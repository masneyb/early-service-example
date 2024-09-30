## Early Service Example

When working in some industries with strict boot time requirements, booting
all the way to the root filesystem is not quick enough to meet the timing
requirements for some use cases. This project shows how to start a
short-lived service in the initial ramdisk (initrd), and pass the running
state to a long-lived service that is started from the root filesystem.

The [example program](early-service.c) has two parts: 1) a timer that
periodically prints and increments a counter, and 2) a server that listens
for commands on a UNIX domain socket. This is wired together like so:

- The [early-service-initrd.service](conf/early-service-initrd.service) systemd
  unit is started inside the initrd and exposes the short-lived server at the
  UNIX domain socket `/run/early-service/early-service.sock`. The
  contents of the `/run` directory in the initrd are also exposed to `/run` on
  the root filesystem.

- Later when systemd is started from the root filesystem,
  [early-service.service](conf/early-service.service) is started. This connects
  to the UNIX domain socket `/run/early-service/early-service.sock` that's
  currently managed by the service running from the initrd, reads the current
  state, the ownership of the file descriptor for the UNIX domain socket is
  passed from the process running in the initrd to the version running on the
  root filesystem, and the initrd process is told to terminate.

- There is a [dracut module](conf/module-setup.sh) that will tell dracut to
  include the necessary files in the initrd.

The UNIX domain socket `/run/early-service/early-service.sock` exists through
the whole state transition.

The following annotated log output from `journalctl` shows the two processes
starting, and exchanging state:

    # early-service-initrd.service is started inside the initrd.
    Apr 05 17:32:16 localhost early-service[323]: Listening on UNIX socket /run/early-service/early-service.sock
    Apr 05 17:32:16 localhost early-service[323]: 0
    Apr 05 17:32:16 localhost early-service[323]: 1
    Apr 05 17:32:16 localhost early-service[323]: 2
    Apr 05 17:32:16 localhost early-service[323]: 3
    Apr 05 17:32:17 localhost early-service[323]: 4
    Apr 05 17:32:17 localhost early-service[323]: 5
    Apr 05 17:32:17 localhost early-service[323]: 6
    Apr 05 17:32:17 localhost early-service[323]: 7
    Apr 05 17:32:17 localhost early-service[323]: 8
    Apr 05 17:32:17 localhost early-service[323]: 9
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:17.649: 10
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:17.749: 11
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:17.850: 12
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:17.950: 13
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:18.050: 14
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:18.072: Passing file descriptor for /run/early-service/early-service.sock to other process
    Apr 05 17:32:18 localhost early-service-initrd[323]: ** Message: 17:32:18.072: Returning counter to client and terminating the process
    Apr 05 17:32:18 localhost early-service[428]: Reading starting position and taking ownership of socket /run/early-service/early-service.sock
    Apr 05 17:32:18 localhost early-service[428]: Successfully received file descriptor for /run/early-service/early-service.sock
    Apr 05 17:32:18 localhost systemd[1]: early-service-initrd.service: Deactivated successfully.
    Apr 05 17:32:18 localhost early-service[428]: 15
    Apr 05 17:32:18 localhost early-service[428]: 16
    Apr 05 17:32:18 localhost early-service[428]: 17
    Apr 05 17:32:18 localhost early-service[428]: 18
    # Only the version started from the root filesystem is now running.

It's intended that you will have some minimal service that runs in the initrd
that does as little as possible, and passes it's state to the fully featured
services running from the root filesystem. The initrd version should only be
running for a few seconds at most.


## Commands available on UNIX domain socket

The following commands are available over the UNIX domain socket at
`/run/early-service/early-service.sock`:

- `get_counter`: return the current counter
- `pass_state_and_terminate`: return the counter and the file descriptor for
  the unix domain socket, and have the process terminate.
- `set_counter ###`: sets the counter to a particular value

You can test the API by using Netcat:

    $ sudo dnf install nc
    $ echo "set_counter 100" | sudo nc -U /run/early-service/early-service.sock
    previous value 502
    $ echo "get_counter" | sudo nc -U /run/early-service/early-service.sock
    137
    $ echo "get_counter" | sudo nc -U /run/early-service/early-service.sock
    141


## Why not start long running services from the initrd?

Here's some reasons why you don't want to have long-running, fully featured
services started in the initrd:

- You can't leak references to any resources on the initrd, otherwise the kernel
  won't be able to free the memory allocated to the initrd when it is unmounted.

- During the initrd to root transition, systemd deletes the contents of the
  tmpfs filesystem containing the contents of the initrd. Your process started
  from the initrd needs to open all of the necessary files before the switch
  root operation happens. This also means that your process, and any associated
  shared libraries, are running files that have been deleted.

- The initrd is a cpio archive, and increasing the size of the initrd is going
  to increase the kernel boot time since it will need to uncompress and extract
  the larger cpio archive.

- Any services started from the initrd will be started before the SELinux
  policy is loaded. Services started from the initrd will run with the
  `kernel_t` label.

- Services started from the initrd can't depend on almost anything like mounts,
  devices, services, dbus, etc so it's difficult to develop software of any
  complexity.

- Adding all of these dependencies to the initrd is only going to move the
  timing bottlenecks booting from the root filesystem to the initrd.


## Building and Usage

The project can be built by running:

    meson build
    cd build
    ninja all

This generates an early-service binary that takes the following arguments:

    $ early-service --help
    Usage:
      early-service [OPTION?] - Example Early Service
    
    Help Options:
      -h, --help                        Show help options
    
    Application Options:
      -d, --timer_delay_ms              Timer delay in milliseconds
      -s, --server_socket_path          Server UNIX domain socket path to listen on
      --survive_systemd_kill_signal     Set argv[0][0] to '@' when running in initrd
      --takeover_existing_socket        Perform socket handoff from initrd to root filesystem


## Integrating this into CentOS Automotive Sample Images

You can use the following commands to build a RPM, and incorporate that
RPM into an image created by the
[Automotive Sample Images](https://gitlab.com/CentOS/automotive/sample-images).

    $ scripts/create-rpm.sh early-service.spec
    $ createrepo_c ~/rpmbuild/RPMS/x86_64/
    $ cd /path/to/sample-images/osbuild-manifests
    $ make DEFINES+='extra_repos=[{"id":"local","baseurl":"file:///home/masneyb/rpmbuild/RPMS/x86_64"}] extra_rpms=["early-service"] image_enabled_services=["early-service"]' cs9-qemu-developer-regular.x86_64.qcow2

If you are incrementally rebuilding the same RPM for testing, then you'll need
to run `dnf clean all` as your regular user to clean the dnf caches before
rerunning the `make` command.
