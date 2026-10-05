#!/bin/sh
# Bring up omi0 after the endpoint module is loaded.
# Address comes from /etc/openmiop.addr (one CIDR, e.g. 10.20.0.13/24).
ADDR=$(cat /etc/openmiop.addr 2>/dev/null) || {
	echo "openmiop: missing /etc/openmiop.addr" >&2
	exit 1
}
insmod /usr/local/lib/openmiop-ep.ko || exit 1
ip link set omi0 mtu 9000 || exit 1
ip link set omi0 up || exit 1
ip addr replace "$ADDR" dev omi0 || exit 1
if command -v nmcli >/dev/null 2>&1; then
	nmcli device set omi0 managed no 2>/dev/null || true
fi
