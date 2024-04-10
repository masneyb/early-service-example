// SPDX-License-Identifier: Apache-2.0

#include <gio/gio.h>
#include <gio/gunixconnection.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>

#define READ_BUFFER_LEN 127

// Command line arguments
static gint timer_delay_ms = 100;
static gchar *server_socket_path;
static gboolean survive_systemd_kill_signal = FALSE;
static gboolean takeover_existing_socket = FALSE;

static GOptionEntry entries[] = {
	{ "timer_delay_ms", 'd', 0, G_OPTION_ARG_INT, &timer_delay_ms,
	  "Timer delay in milliseconds", NULL, },
	{ "server_socket_path", 's', 0, G_OPTION_ARG_FILENAME,
	  &server_socket_path, "Server UNIX domain socket path to listen on",
	  NULL, },
	{ "survive_systemd_kill_signal", 0, 0, G_OPTION_ARG_NONE,
	  &survive_systemd_kill_signal,
	  "Set argv[0][0] to '@' when running in initrd", NULL },
	{ "takeover_existing_socket", 0, 0, G_OPTION_ARG_NONE,
	  &takeover_existing_socket,
	  "Perform socket handoff from initrd to root filesystem", NULL },
	{ NULL }
};

struct service_info {
	GSocket *main_socket;
	GMainLoop *loop;
	int counter;
};

struct connection_info {
	GSocketConnection *connection;
	struct service_info *service_info;
	char buf[50];
	gboolean terminate;
};

static gboolean timer_callback(gpointer data)
{
	struct service_info *service_info = data;

	g_message("%d", service_info->counter++);

	return G_SOURCE_CONTINUE;
}

/*
 * The next block of functions are for the server that's exposed on a UNIX
 * domain socket. This is all done with asynchronous IO so that nothing will
 * block the glib main loop.
 */

void server_free_connection(struct connection_info *conn)
{
	g_object_unref(G_SOCKET_CONNECTION(conn->connection));
	g_free(conn);
}

void server_message_sent(GObject *source_object, GAsyncResult *res,
			 gpointer user_data)
{
	GOutputStream *ostream = G_OUTPUT_STREAM(source_object);
	struct connection_info *conn = user_data;
	gboolean terminate = conn->terminate;
	g_autoptr(GError) error = NULL;
	gboolean success;

	success = g_output_stream_write_all_finish(ostream, res, NULL, &error);
	if (error != NULL) {
		g_printerr("%s", error->message);
		server_free_connection(conn);
		return;
	} else if (!success) {
		server_free_connection(conn);
		return;
	}

	if (terminate) {
		g_main_loop_quit(conn->service_info->loop);
		server_free_connection(conn);
	}
}

void server_send_message(struct connection_info *conn)
{
	g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(conn->connection)),
					conn->buf, strlen(conn->buf),
					G_PRIORITY_DEFAULT, NULL,
					server_message_sent, conn);
}

#define SERVER_SET_COUNTER_COMMAND "set_counter "

void server_message_ready(GObject *source_object, GAsyncResult *res,
			  gpointer user_data)
{
	GInputStream *istream = G_INPUT_STREAM(source_object);
	struct connection_info *conn = user_data;
	g_autoptr(GError) error = NULL;
	int new_counter;
	gssize count;
	char *pos;

	count = g_input_stream_read_finish(istream, res, &error);
	if (error != NULL) {
		g_printerr("%s", error->message);
		server_free_connection(conn);
		return;
	} else if (count == 0) {
		server_free_connection(conn);
		return;
	}

	/*
	 * Note that this doesn't properly handle buffers containing
	 * multiple commands, or where a single command spans multiple
	 * buffers. This assumes one command per buffer since this
	 * program is just a proof of concept.
	 */
	conn->buf[sizeof(conn->buf) - 1] = '\0';
	if ((pos = strchr(conn->buf, '\n')) != NULL)
		*pos = '\0';

	if (g_str_equal(conn->buf, "get_counter")) {
		g_message("Returning counter to client");

		g_snprintf(conn->buf, sizeof(conn->buf),
			   "%d\n", conn->service_info->counter);
		server_send_message(conn);
	} else if (g_str_equal(conn->buf, "pass_state_and_terminate")) {
		g_message("Passing file descriptor for %s to other process",
			  server_socket_path);

		g_unix_connection_send_fd(G_UNIX_CONNECTION(conn->connection),
					  g_socket_get_fd(conn->service_info->main_socket),
					  NULL, &error);

		g_message("Returning counter to client and terminating the process");

		g_snprintf(conn->buf, sizeof(conn->buf),
			   "%d\n", conn->service_info->counter);
		server_send_message(conn);

		conn->terminate = TRUE;
	} else if (g_str_has_prefix(conn->buf,
				    SERVER_SET_COUNTER_COMMAND)) {
		new_counter = g_ascii_strtoll(conn->buf + sizeof(SERVER_SET_COUNTER_COMMAND) - 1,
					      NULL, 10);

		g_message("Setting the counter to %d", new_counter);

		g_snprintf(conn->buf, sizeof(conn->buf),
			   "previous value %d\n", conn->service_info->counter);
		conn->service_info->counter = new_counter;
		server_send_message(conn);
	} else {
		g_message("Unknown message '%s' from client", conn->buf);

		g_snprintf(conn->buf, sizeof(conn->buf), "Invalid command\n");
		server_send_message(conn);
	}

	g_input_stream_read_async(istream, conn->buf, sizeof(conn->buf),
				  G_PRIORITY_DEFAULT, NULL,
				  server_message_ready, conn);
}

static gboolean server_incoming_connection(GSocketService *service,
					   GSocketConnection *connection,
					   GObject *source_object,
					   gpointer user_data)
{
	struct connection_info *conn = g_new0(struct connection_info, 1);

	conn->connection = g_object_ref(connection);
	conn->service_info = user_data;

	g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
				  conn->buf, sizeof(conn->buf),
				  G_PRIORITY_DEFAULT, NULL,
				  server_message_ready, conn);

	return FALSE;
}

static void
server_event_cb(GSocketListener *listener, GSocketListenerEvent event,
		GSocket *socket, gpointer user_data)
{
	struct service_info *service_info = user_data;

	/*
	 * We need the GSocket for the main server so that the file descriptor
	 * can be passed to another process if needed. This is only available
	 * within glib in a private structure, so add an event callback to
	 * grab the GSocket.
	 */
	if (event == G_SOCKET_LISTENER_LISTENED)
		service_info->main_socket = g_object_ref(socket);
}

static GSocketService *create_unix_domain_server(char *server_socket_path,
						 struct service_info *service_info)
{
	g_autoptr(GSocketService) service = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GError) error = NULL;

	service = g_socket_service_new();
	if (service == NULL) {
		g_printerr("Error creating socket service.\n");
		return NULL;
	}

	g_signal_connect(G_SOCKET_LISTENER(service), "event",
			 G_CALLBACK(server_event_cb), service_info);

	address = g_unix_socket_address_new(server_socket_path);
	if (address == NULL) {
		g_printerr("Error creating socket address.\n");
		return NULL;
	}

	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service),
					   G_SOCKET_ADDRESS(address),
					   G_SOCKET_TYPE_STREAM,
					   G_SOCKET_PROTOCOL_DEFAULT,
					   NULL, NULL, &error)) {
		g_printerr("Error binding socket: %s\n", error->message);
		return NULL;
	}

	g_signal_connect(service, "incoming",
			 G_CALLBACK(server_incoming_connection), service_info);

	g_socket_service_start(service);

	return g_steal_pointer(&service);
}

static GSocketService *create_service_from_existing_fd(int existing_fd,
						       struct service_info *service_info)
{
	g_autoptr(GSocketService) service = NULL;
	g_autoptr(GSocket) socket = NULL;
	g_autoptr(GError) error = NULL;

	socket = g_socket_new_from_fd(existing_fd, &error);
	if (error != NULL) {
		g_printerr("Error creating socket from fd: %s",
			   error->message);
		return NULL;
	}

	service = g_socket_service_new();
	if (service == NULL) {
		g_printerr("Error creating socket service.\n");
		return NULL;
	}

	g_socket_listener_add_socket(G_SOCKET_LISTENER(service), socket, NULL,
				     &error);
	if (error != NULL) {
		g_printerr("Error binding socket: %s", error->message);
		return NULL;
	}

	g_signal_connect(service, "incoming",
			 G_CALLBACK(server_incoming_connection), service_info);

	g_socket_service_start(service);

	service_info->main_socket = g_steal_pointer(&socket);

	return g_steal_pointer(&service);
}

/*
 * This is the client that reads the current state from another process
 * via a UNIX domain socket. This is done using synchronous IO since this
 * is only called on boot up and we will be blocked waiting to read the
 * current state.
 */

#define CLIENT_GET_COUNTER_COMMAND "pass_state_and_terminate\n"

gboolean read_counter_and_fd_from_server(int *existing_counter, int *fd)
{
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GError) error = NULL;
	gssize bytes_read;
	gchar buf[100];

	client = g_socket_client_new();
	address = g_unix_socket_address_new(server_socket_path);

	connection = g_socket_client_connect(client,
					     G_SOCKET_CONNECTABLE(address),
					     NULL, &error);
	if (error != NULL) {
		g_printerr("Error connecting to socket: %s", error->message);
		return FALSE;
	}

	GInputStream *input_stream = g_io_stream_get_input_stream(G_IO_STREAM(connection));
	GOutputStream *output_stream = g_io_stream_get_output_stream(G_IO_STREAM(connection));

	g_output_stream_write(output_stream, CLIENT_GET_COUNTER_COMMAND,
			      strlen(CLIENT_GET_COUNTER_COMMAND), NULL, &error);
	if (error != NULL) {
		g_printerr("Error writing to socket: %s", error->message);
		return FALSE;
	}

	*fd = g_unix_connection_receive_fd(G_UNIX_CONNECTION(connection), NULL,
					   &error);
	if (error != NULL) {
		g_printerr("Error receiving file descriptor: %s",
			   error->message);
		return FALSE;
	} else if (*fd < 0) {
		g_printerr("Received invalid file descriptor");
		return FALSE;
	}

	g_message("Successfully received file descriptor for %s",
		  server_socket_path);

	bytes_read = g_input_stream_read(input_stream, buf, sizeof(buf) - 1,
					 NULL, &error);
	if (error != NULL) {
		g_printerr("Error reading from socket: %s", error->message);
		return FALSE;
	}

	buf[bytes_read] = '\0';

	*existing_counter = g_ascii_strtoll(buf, NULL, 10);

	return TRUE;
}

GSocketService *create_service(struct service_info *service_info)
{
	g_autoptr(GSocketService) service = NULL;
	int existing_fd;

	if (takeover_existing_socket) {
		g_message("Reading starting position and taking ownership of socket %s",
			  server_socket_path);

		if (read_counter_and_fd_from_server(&service_info->counter,
						    &existing_fd)) {
			service = create_service_from_existing_fd(existing_fd,
								  service_info);
			if (service != NULL)
				return g_steal_pointer(&service);
		}

		/*
		 * There was some kind of error taking over the existing
		 * service, so error out here. A safer option would be to
		 * not exit and let it fall through to the code below so
		 * that a new server is created.
		 */
		exit(1);
	}

	g_message("Listening on UNIX socket %s", server_socket_path);
	service = create_unix_domain_server(server_socket_path, service_info);
	if (service == NULL)
		exit(1);

	return g_steal_pointer(&service);
}

int main(int argc, char **argv)
{
	g_autoptr(GSocketService) service = NULL;
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	struct service_info server = {
		.main_socket = NULL,
		.counter = 0
	};

	context = g_option_context_new("- Example Early Service");
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("option parsing failed: %s\n", error->message);
		return 1;
	}

	if (survive_systemd_kill_signal) {
		/*
		 * See https://systemd.io/ROOT_STORAGE_DAEMONS/ for more details
		 * about having this started inside the initrd running when the
		 * system transitions to running services from the root
		 * filesystem. systemd v255 and higher has the option
		 * SurviveFinalKillSignal=yes that can be used instead.
		 */
		argv[0][0] = '@';
	}

	server.loop = g_main_loop_new(NULL, FALSE);

	guint timer_id = g_timeout_add(timer_delay_ms, timer_callback, &server);

	if (server_socket_path != NULL)
		service = create_service(&server);
	else
		g_message("Not listening on a UNIX socket.");

	g_main_loop_run(server.loop);

	g_source_remove(timer_id);
	if (service != NULL) {
		g_socket_service_stop(service);
		g_socket_listener_close(G_SOCKET_LISTENER(service));
		g_object_unref(server.main_socket);
	}

	g_main_loop_unref(server.loop);

	return 0;
}
