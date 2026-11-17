#!/bin/bash
# Scenario test of the screen lock (awl_lockscreen.c) with a real
# ext-session-lock client (test/lock.c) in a live.sh test instance: a
# locker that doesn't lock in time is stopped (the lock helper held up, so
# awlock never starts); the client locks every output; a locker that
# crashes leaves awl locked and alive, and awl starts its own, awlock, up to
# 5 times until an unlock; a new locker can lock again; and awl exits
# cleanly on SIGTERM while a lock is in place (the lock then goes with its
# client, after lockfini() has removed the lock's timers).
#
# awlock is the instance's, found by its parents (awl, the lock helper),
# never by name alone: the real session's must be left alone. It locks;
# nothing types a password.
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

# the instance's lock helper, and its awlock (none: empty)
helper() { pgrep -P "$("$live" pid)" -x awl-lockhelper; }
awlock() { pgrep -P "$(helper)" -x awlock; }

# waits ($3 s at most, by default 5) for the log to have $1 lines matching $2
logged() {
	for _ in $(seq $((${3:-5} * 10))); do
		[ "$("$live" log | grep -c -- "$2")" -ge "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

# waits (5 s at most) for an awlock other than $1 to run
newawlock() {
	local p
	for _ in $(seq 50); do
		p=$(awlock)
		[ -n "$p" ] && [ "$p" != "${1:-}" ] && return 0
		sleep 0.1
	done
	return 1
}

ninja -C "$builddir" test/lock >/dev/null || exit 1
"$live" stop >/dev/null 2>&1
"$live" start >/dev/null || die "awl didn't start"

echo "-- a locker that doesn't lock"
h=$(helper) || die "no lock helper"
kill -STOP "$h"
"$live" win locker >/dev/null
check "it is stopped after 10 s" logged 1 "awlock didn't lock in 10000 ms" 15
kill -CONT "$h"
check "awl survives it" "$live" alive
"$live" alive || die "awl died"
sleep 0.5
check "the awlock that started late is gone" test -z "$(awlock)"

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
check "awl starts awlock" newawlock
check "and says so" logged 1 "starting another (1 of 5)"

echo "-- awlock crashes"
for i in 2 3 4 5 6; do
	p=$(awlock) || die "no awlock"
	kill -KILL "$p"
	[ $i -le 5 ] && check "awl starts awlock again" newawlock "$p"
done
check "5 times at most" logged 1 "started already"
sleep 0.5
check "then none" test -z "$(awlock)"
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
