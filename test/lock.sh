#!/bin/bash
# Scenario test of the screen lock (awl_lockscreen.c) with a real
# ext-session-lock client (test/lock.c) in a live.sh test instance: it
# locks every output; a locker that crashes leaves awl locked and alive,
# and a new one can lock again; and awl exits cleanly on SIGTERM while a
# lock is in place (the lock then goes with its client, after lockfini()
# has removed the lock's timers).
#
#   test/lock.sh        with build-dev/awl (the debug build) if it is there,
#                       else build/awl
#
# LIVE_AWL picks the binary. The state goes to $LIVE_DIR, by default
# /tmp/awl-lock-$UID. Exit status: the number of failed checks (100: awl
# died, the rest was skipped).
set -u
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
live=$here/live.sh
export LIVE_DIR=${LIVE_DIR:-/tmp/awl-lock-$UID}
[ -x "$src/build-dev/awl" ] && export LIVE_AWL=${LIVE_AWL:-build-dev/awl}
export LIVE_OUTPUTS=${LIVE_OUTPUTS:-2}
awl=${LIVE_AWL:-build/awl}
case $awl in /*) ;; *) awl=$src/$awl ;; esac
builddir=$(dirname "$awl")
fails=0
lockers=()

check() {
	local d=$1
	shift
	if "$@"; then
		echo "ok    $d"
	else
		echo "FAIL  $d"
		fails=$((fails + 1))
	fi
}

die() {
	echo "FATAL $*, log: $LIVE_DIR/awl.log"
	"$live" log | tail -40
	"$live" stop >/dev/null 2>&1
	for p in "${lockers[@]}"; do kill "$p" 2>/dev/null; done
	exit 100
}

# starts a locker, its output into $LIVE_DIR/lockN.out; waits (5 s at most)
# for it to say it locked
locker() {
	local out=$LIVE_DIR/lock$1.out
	"$live" run "$builddir/test/lock" >"$out" 2>&1 &
	lockers+=($!)
	for _ in $(seq 50); do
		grep -qx locked "$out" && return 0
		sleep 0.1
	done
	return 1
}

ninja -C "$builddir" test/lock >/dev/null || exit 1
"$live" stop >/dev/null 2>&1
"$live" start >/dev/null || die "awl didn't start"

echo "-- lock"
check "the locker locks every output" locker 1
"$live" alive || die "awl died"

echo "-- the locker crashes"
# the client itself, not live.sh's shell around it
pkill -KILL -x -P "${lockers[0]}" lock || die "no locker to kill"
wait "${lockers[0]}" 2>/dev/null
sleep 0.5
check "awl survives it" "$live" alive
"$live" alive || die "awl died"
check "a new locker locks again" locker 2
"$live" alive || die "awl died"

echo "-- exit while locked"
out=$("$live" stop)
check "clean exit with the lock in place" grep -q "exited normally" <<<"$out"
wait "${lockers[1]}"
check "the locker held the lock until awl went" test $? -eq 0
[ $fails -eq 0 ] || echo "log: $LIVE_DIR/awl.log"
echo "$fails failed"
exit $fails
