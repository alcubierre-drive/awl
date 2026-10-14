#!/bin/bash
# live.sh, but with NetworkManager: the test instance's system bus is an
# xdg-dbus-proxy in front of the real one ($LIVE_NM_UPSTREAM, default
# unix:path=/run/dbus/system_bus_socket) that lets through only what
# plugins/nm.c does, reading NetworkManager's properties and receiving their
# PropertiesChanged signals. Everything else on the system bus, logind
# included, stays invisible, so the test instance can't take inhibitors or
# change the real session (and raises the logind alert, as without it).
#
# Same commands as live.sh; start also starts the proxy, stop also stops it.
set -u
here=$(cd "$(dirname "$0")" && pwd)
dir=${LIVE_DIR:-/tmp/awl-live-$UID}
sock=$dir/system_bus_nm
nm=org.freedesktop.NetworkManager
path=/org/freedesktop/NetworkManager
props=org.freedesktop.DBus.Properties

stopproxy() {
	[ -f "$dir/proxy.pid" ] && kill "$(cat "$dir/proxy.pid")" 2>/dev/null
	rm -f "$dir/proxy.pid" "$sock"
}

case ${1:-} in
start)
	mkdir -p "$dir"
	stopproxy
	setsid xdg-dbus-proxy "${LIVE_NM_UPSTREAM:-unix:path=/run/dbus/system_bus_socket}" "$sock" \
		--filter --see=$nm \
		--call=$nm=$props.Get@$path --call=$nm=$props.Get@$path/* \
		--call=$nm=$props.GetAll@$path --call=$nm=$props.GetAll@$path/* \
		--broadcast=$nm=$props.PropertiesChanged@$path \
		--broadcast=$nm=$props.PropertiesChanged@$path/* \
		>"$dir/proxy.log" 2>&1 </dev/null &
	echo $! >"$dir/proxy.pid"
	for i in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
	[ -S "$sock" ] || { echo "no proxy, see $dir/proxy.log" >&2; stopproxy; exit 1; }
	LIVE_SYSTEM_BUS=unix:path=$sock exec "$here/live.sh" "$@"
	;;
stop)
	"$here/live.sh" "$@"
	r=$?
	stopproxy
	exit $r
	;;
*) exec "$here/live.sh" "$@" ;;
esac
