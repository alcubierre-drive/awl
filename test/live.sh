#!/bin/bash
# Drive a test instance of ./awl from a shell, e.g. from an agent without a
# screen: headless outputs, its own runtime dir and D-Bus session (so it can't
# disturb the running session's tray, notifications or sockets) and a fake
# $HOME whose Desktop is a fixture. It runs under gdb, which dumps every
# thread's backtrace into the log if it crashes.
#
#   live.sh start [ARGS]    start, wait for the socket (LIVE_TIMEOUT s max);
#                           ARGS go to awl, e.g. -d for its verbose log
#                           (where the popups' pointer input goes, ...)
#   live.sh stop            SIGTERM, wait for it to exit
#   live.sh alive           exit status: is it running
#   live.sh add | remove    add an output / remove the newest (AWL_TEST_OUTPUTS)
#   live.sh shot FILE       screenshot of all outputs (grim)
#   live.sh randr [ARGS]    wlr-randr
#   live.sh key ARGS        wtype, e.g. key -M alt -M ctrl r -m ctrl -m alt
#   live.sh click X Y [BUTTON...]  move the pointer there in layout pixels and
#                           click left|middle|right, or scroll wheel:N (N
#                           notches down, negative up), hwheel:N (right),
#                           finger:D (a touchpad's D down), see vptr.c
#   live.sh win [CMD]       a command for awl's text popups (AWL_TEST_WINDOWS,
#                           see testwincmd() in awl.c), e.g. win show 0
#                           HEADLESS-1 640 'a\tb\nc'; then prints their state
#   live.sh run CMD...      any client, with WAYLAND_DISPLAY etc. set
#   live.sh log             the log so far
#
# LIVE_OUTPUTS=N starts with N outputs (1). LIVE_NESTED=1 makes them windows
# in the session at $WAYLAND_DISPLAY instead of headless ones; that session's
# keyboard and pointer then work in them too.
#
# The system bus is $LIVE_SYSTEM_BUS (default unix:path=$LIVE_DIR/system_bus,
# where nothing listens unless a test starts its own dbus-daemon there): the
# logind thread takes inhibitors and sets the session's LockedHint, so it must
# never reach the real logind; NetworkManager is gone with it. live-nm.sh
# brings it back.
#
# State goes to $LIVE_DIR (default /tmp/awl-live-$UID): log, runtime dir,
# home. awl gets -l '' (stderr, i.e. that log, only), so it leaves the real
# awl's log file alone; arguments after start come after it. Desktop fixture:
# $LIVE_DIR/home/Desktop, created on start if missing.
#
# LIVE_AWL=PATH runs another binary, relative to the source dir, e.g.
# LIVE_AWL=dev/awl for make dev's unoptimized debug build, which picks up
# dev/libawlplugins.so.
set -u
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
awl=${LIVE_AWL:-awl}
case $awl in /*) ;; *) awl=$src/$awl ;; esac
dir=${LIVE_DIR:-/tmp/awl-live-$UID}
run=$dir/run
log=$dir/awl.log

envs() {
	export XDG_RUNTIME_DIR=$run HOME=$dir/home
	export WAYLAND_DISPLAY=$(cat "$dir/socket" 2>/dev/null)
	unset DISPLAY
}

pid() { # awl's, not gdb's; everything runs in the session start made
	local sid
	sid=$(cat "$dir/sid" 2>/dev/null) || return 1
	pgrep -s "$sid" -x awl
}

# the (union of) output layout size, for absolute pointer motion
extent() {
	envs
	wlr-randr 2>/dev/null | awk '
		/^[^ ]/ { on = 0 }
		/Enabled: yes/ { on = 1 }
		/current/ { split($1, s, "x"); w = s[1]; h = s[2] }
		/Position:/ { split($2, p, ","); px = p[1]; py = p[2] }
		/Scale:/ { if (on) { sc = $2; r = px + w / sc; b = py + h / sc
			if (r > W) W = r; if (b > H) H = b } }
		END { printf "%dx%d\n", W, H }'
}

fixture() {
	local d=$dir/home/Desktop
	[ -d "$d" ] && return
	mkdir -p "$d/Projects" "$d/.hidden_dir"
	touch "$d/notes.txt" "$d/report.pdf" "$d/.hidden_file"
	ln -s /nonexistent "$d/broken_link"
}

# Only the real home's fonts, fontconfig, icon and GTK themes: anything
# started in the test session (autostarts, D-Bus activated services) must not
# see the user's real application data.
homelinks() {
	local f
	for f in .config .local .cache; do # older versions linked these whole
		[ -L "$dir/home/$f" ] && rm "$dir/home/$f"
	done
	for f in .fonts .icons .themes .config/fontconfig .config/gtk-3.0 .config/gtk-4.0 \
			.local/share/fonts .local/share/icons .local/share/themes; do
		[ -e "$HOME/$f" ] || continue
		mkdir -p "$(dirname "$dir/home/$f")"
		ln -sfn "$HOME/$f" "$dir/home/$f"
	done
}

case ${1:-} in
start)
	pid >/dev/null && { echo "already running" >&2; exit 1; }
	[ -x "$awl" ] || { echo "no $awl (make dev for dev/awl)" >&2; exit 1; }
	mkdir -p "$run" && chmod 700 "$run"
	rm -f "$run"/wayland-* "$dir/socket" "$dir/ctl"
	mkfifo "$dir/ctl"
	fixture
	homelinks
	(
		# audio for the volume widget; libpulse refuses a symlinked runtime dir
		[ -S "$XDG_RUNTIME_DIR/pulse/native" ] && export PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
		if [ "${LIVE_NESTED:-}" ]; then
			# windows in the running session; an absolute path, since
			# XDG_RUNTIME_DIR changes below
			case ${WAYLAND_DISPLAY:-} in
			"") echo "LIVE_NESTED needs WAYLAND_DISPLAY" >&2; exit 1 ;;
			/*) outer=$WAYLAND_DISPLAY ;;
			*) outer=$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY ;;
			esac
			export WLR_BACKENDS=wayland WAYLAND_DISPLAY=$outer WLR_WL_OUTPUTS=${LIVE_OUTPUTS:-1}
		else
			unset WAYLAND_DISPLAY
			export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=${LIVE_OUTPUTS:-1}
		fi
		export XDG_RUNTIME_DIR=$run HOME=$dir/home
		export DBUS_SYSTEM_BUS_ADDRESS=${LIVE_SYSTEM_BUS:-unix:path=$dir/system_bus}
		unset DISPLAY WAYLAND_SOCKET
		export WLR_RENDERER=${WLR_RENDERER:-gles2} WLR_LIBINPUT_NO_DEVICES=1 AWL_TEST_OUTPUTS=1
		export AWL_TEST_WINDOWS=$dir/ctl
		cd "$dir"
		exec setsid timeout -s TERM "${LIVE_TIMEOUT:-600}" dbus-run-session -- \
			gdb -q -batch \
			-ex "handle SIGUSR1 SIGUSR2 SIGTERM SIGPIPE nostop noprint pass" \
			-ex run -ex "bt full" -ex "thread apply all bt" \
			--args "$awl" -l '' "${@:2}"
	) >"$log" 2>&1 </dev/null &
	echo $! >"$dir/sid" # setsid didn't need to fork, $! is the session leader
	for i in $(seq 100); do
		s=$(cd "$run" && ls wayland-[0-9]* 2>/dev/null | grep -v lock | head -1)
		[ -n "$s" ] && break
		sleep 0.1
	done
	[ -n "$s" ] || { echo "no socket, see $log" >&2; exit 1; }
	echo "$s" >"$dir/socket"
	sleep 0.5 # the first frame
	echo "running, socket $run/$s, log $log"
	;;
stop)
	p=$(pid) || { echo "not running" >&2; exit 1; }
	kill -TERM "$p"
	for i in $(seq 50); do pid >/dev/null || break; sleep 0.1; done
	pid >/dev/null && { echo "still running after 5 s" >&2; exit 1; }
	# gdb prints these on a clean exit, a crash shows a backtrace instead
	grep -E "exited (normally|with code)|received signal" "$log" | tail -1
	;;
alive) pid >/dev/null ;;
add) kill -USR1 "$(pid)" && sleep 0.3 ;;
remove) kill -USR2 "$(pid)" && sleep 0.3 ;;
shot) envs; grim "$2" ;;
randr) envs; shift; wlr-randr "$@" ;;
key) envs; shift; wtype "$@" ;;
click)
	e=$(extent)
	envs; shift
	make -s -C "$here" vptr >&2 || exit 1
	VPTR_EXTENT=$e "$here/vptr" "$@"
	;;
win)
	shift
	pid >/dev/null || { echo "not running" >&2; exit 1; }
	# the dump comes after the command, in the same event loop round, so it
	# shows its effect (a restart's only once the loop is idle: win again)
	rm -f "$dir/windows"
	printf '%s\ndump %s\n' "$*" "$dir/windows" >"$dir/ctl"
	for i in $(seq 50); do [ -e "$dir/windows" ] && break; sleep 0.1; done
	cat "$dir/windows"
	;;
run) envs; shift; "$@" ;;
log) cat "$log" ;;
*) sed -n '2,/^set/p' "$0" | sed '$d;s/^# \?//'; exit 2 ;;
esac
