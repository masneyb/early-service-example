#!/usr/bin/bash
# SPDX-License-Identifier: Apache-2.0

# called by dracut
depends() {
	echo "systemd"
}

# called by dracut
install() {
	# Only enable the early-service in the initrd if it's enabled on the root
	if systemctl --quiet --root "$dracutsysrootdir" is-enabled early-service.service ; then
		grep '^early-service:' "$dracutsysrootdir/etc/passwd" >> "$initdir/etc/passwd"
		grep '^early-service:' "$dracutsysrootdir/etc/group" >> "$initdir/etc/group"

		inst_simple "${systemdsystemunitdir}/early-service-initrd.service" "${systemdsystemunitdir}/early-service-initrd.service"
		inst_binary /usr/bin/early-service /usr/bin/early-service

		$SYSTEMCTL -q --root "$initdir" enable early-service-initrd.service
	fi
}
