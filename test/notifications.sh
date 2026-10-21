#!/bin/bash
# Scenario test of awl's notification server (awl_notifications.c): a
# live.sh test instance, its own session bus, driven by gdbus as clients
# would and by AWL_TEST_WINDOWS' nclose/ninvoke/nhold as the library would
# (testwincmd() in awl_test.c), checking the open notifications in its dump
# and the signals a gdbus monitor saw; at the end that awl is still alive
# and exits cleanly.
#
#   test/notifications.sh   with build-dev/awl (the debug build) if it is
#                           there, else build/awl
#
# LIVE_AWL picks the binary. The state goes to $LIVE_DIR, by default
# /tmp/awl-notifications-$UID. Exit status: the number of failed checks
# (100: awl died, the rest was skipped).
set -u
here=$(cd "$(dirname "$0")" && pwd)
live=$here/live.sh
export LIVE_DIR=${LIVE_DIR:-/tmp/awl-notifications-$UID}
[ -x "$here/../build-dev/awl" ] && export LIVE_AWL=${LIVE_AWL:-build-dev/awl}
fails=0
state=
signals=$LIVE_DIR/signals
pids=()

die() {
	echo "FATAL $*, log: $LIVE_DIR/awl.log"
	"$live" log | tail -40
	[ -n "${monitor:-}" ] && kill "$monitor" 2>/dev/null
	"$live" stop >/dev/null 2>&1
	exit 100
}

check() {
	local d=$1
	shift
	if "$@"; then
		echo "ok    $d"
	else
		echo "FAIL  $d"
		grep '^notif ' <<<"$state" | head -8 | sed 's/^/        /'
		fails=$((fails + 1))
	fi
}

# the dump's notif lines into $state
look() {
	"$live" alive || die "awl died"
	state=$("$live" win "${@:-}") && [ -n "$state" ] || die "no state from awl"
}

call() {
	"$live" run gdbus call --session --dest org.freedesktop.Notifications \
		--object-path /org/freedesktop/Notifications \
		--method "org.freedesktop.Notifications.$1" "${@:2}"
}

# the id in Notify's answer, "(uint32 ID,)"
id() { sed -n 's/^(uint32 \([0-9]*\),)$/\1/p'; }

# notify APP REPLACES SUMMARY ACTIONS HINTS TIMEOUT: prints the id
notify() {
	call Notify "$1" "$2" '' "$3" body "$4" "$5" "int32 $6" | id
}

# column C of notification ID's line (2 the id, 3 the generation, 4 urgency,
# 5 timeout, 6 value, 7 flags, 8 actions, 9 image, 10 image-path, 11
# app_icon, 12 category, 13 the summary)
field() { awk -v id="$1" -v c="$2" '$1 == "notif" && $2 == id { print $c }' <<<"$state"; }
open_() { grep -q "^notif $1 " <<<"$state"; }
closed() { ! open_ "$1"; }
count() { grep -c '^notif ' <<<"$state"; }
eq() { [ "$1" = "$2" ]; }
# the place of ID among the open ones, newest first, from 1
place() { grep '^notif ' <<<"$state" | awk -v id="$1" '$2 == id { print NR }'; }
# a signal the monitor saw: closed ID REASON, action ID KEY, token ID TOKEN
sawclosed() { grep -q "NotificationClosed (uint32 $1, uint32 $2)" "$signals"; }
sawaction() { grep -q "ActionInvoked (uint32 $1, '$2')" "$signals"; }
sawtoken() { grep -q "ActivationToken (uint32 $1, '$2')" "$signals"; }

"$live" stop >/dev/null 2>&1
"$live" start -d >/dev/null || die "no start"
for i in $(seq 50); do
	"$live" log | grep -q "notifications: running" && break
	sleep 0.1
done
"$live" run gdbus monitor --session --dest org.freedesktop.Notifications >"$signals" 2>&1 &
monitor=$!
sleep 0.5

echo "-- server"
info=$(call GetServerInformation)
check "server information" eq "${info%%,*}" "('awl'"
check "spec version 1.2" grep -q "'1.2')" <<<"$info"
check "capabilities" grep -q "'body', 'body-markup', 'actions', 'icon-static'" <<<"$(call GetCapabilities)"

echo "-- notify"
a=$(notify app 0 first '[]' '{}' -1)
b=$(notify app 0 second '[]' '{"urgency": <byte 0>, "category": <"im">}' -1)
c=$(notify app 0 third '[]' '{"urgency": <byte 2>}' 3000)
look
check "ids grow" [ -n "$a" ] && [ "$b" -gt "$a" ] && [ "$c" -gt "$b" ]
check "newest first" eq "$(place "$c")$(place "$b")$(place "$a")" 123
check "normal: the config's timeout" eq "$(field "$a" 5)" 10000
check "low: the config's timeout, category" eq "$(field "$b" 5) $(field "$b" 12)" "5000 im"
check "critical: none, even when asked" eq "$(field "$c" 4) $(field "$c" 5)" "2 0"
d=$(notify app 0 never '[]' '{}' 0)
look
check "0: no timeout" eq "$(field "$d" 5)" 0

echo "-- replace"
g=$(field "$a" 3)
r=$(notify app "$a" first2 '[]' '{"value": <uint32 150>}' -1)
look
check "replaces_id keeps the id" eq "$r" "$a"
check "in place, generation grows" [ "$(place "$a")" = 4 ] && [ "$(field "$a" 3)" -gt "$g" ]
check "the new contents, value clamped" eq "$(field "$a" 13) $(field "$a" 6)" "first2 100"
call CloseNotification "$d" >/dev/null
r=$(notify app "$d" again '[]' '{}' -1)
look
check "a closed id comes back as itself" eq "$r" "$d"
check "it is open again, newest" eq "$(place "$d")" 1
r=$(notify app 4000000000 unknown '[]' '{}' -1)
check "an id never given out isn't taken" [ -n "$r" ] && [ "$r" != 4000000000 ]
t1=$(notify vol 0 v1 '[]' '{"x-dunst-stack-tag": <"volume">, "value": <byte 10>}' -1)
t2=$(notify vol 0 v2 '[]' '{"x-dunst-stack-tag": <"volume">, "value": <byte 20>}' -1)
t3=$(notify other 0 v3 '[]' '{"x-canonical-private-synchronous": <"volume">}' -1)
look
check "a stack tag replaces" eq "$t2 $(field "$t1" 13) $(field "$t1" 6)" "$t1 v2 20"
check "only the same app's" [ "$t3" != "$t1" ]

echo "-- close and expire"
call CloseNotification "$b" >/dev/null
call CloseNotification 99999 >/dev/null
e=$(notify app 0 short '[]' '{}' 300)
h=$(notify app 0 held '[]' '{}' 300)
look "nhold $h 1"
sleep 0.8
look
check "CloseNotification closes" closed "$b"
check "expired" closed "$e"
check "held doesn't expire" open_ "$h"
look "nhold $h 0"
sleep 0.8
look
check "released, it expires" closed "$h"
look "nclose $a 2"
check "the library dismisses" closed "$a"

echo "-- actions"
x=$(notify app 0 act '["default", "Open", "reply", "Reply"]' '{}' 0)
y=$(notify app 0 res '["default", "Open"]' '{"resident": <true>}' 0)
look "ninvoke $x nosuch"
check "an unknown key does nothing" open_ "$x"
look "ninvoke $x reply tok123"
check "an action closes it" closed "$x"
look "ninvoke $y default"
check "unless resident" eq "$(field "$y" 7) $(field "$y" 8)" "2 1"
z=$(notify app 0 odd '["a", "A", "b"]' '{"transient": <int32 1>}' 0)
look
check "the odd one out dropped, transient" eq "$(field "$z" 8) $(field "$z" 7)" "1 1"

echo "-- hints"
px='[byte 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]'
i1=$(notify app 0 img '[]' "{\"image-data\": <(2,2,8,true,8,4,$px)>}" 0)
i2=$(notify app 0 rgb '[]' "{\"image_data\": <(2,2,7,false,8,3,[byte 1,2,3,4,5,6,0,7,8,9,10,11,12])>}" 0)
i3=$(notify app 0 prio '[]' "{\"image-data\": <(1,1,4,true,8,4,[byte 1,2,3,4])>, \"icon_data\": <(2,2,8,true,8,4,$px)>}" 0)
i4=$(notify app 0 short '[]' "{\"image-data\": <(2,2,8,true,8,4,[byte 1,2,3])>}" 0)
i5=$(notify app 0 chan '[]' "{\"image-data\": <(2,2,8,false,8,4,$px)>}" 0)
i6=$(notify app 0 bits '[]' "{\"image-data\": <(2,2,8,true,16,4,$px)>}" 0)
i7=$(notify app 0 big '[]' "{\"image-data\": <(2000,1,8000,true,8,4,$px)>}" 0)
i8=$(notify app 0 neg '[]' "{\"image-data\": <(2,2,-8,true,8,4,$px)>}" 0)
i9=$(notify app 0 type '[]' '{"image-data": <"x">, "urgency": <"high">, "value": <"5">}' 0)
p1=$(call Notify app 0 dialog-information path body '[]' '{"image-path": <"/tmp/a b.png">}' 'int32 0' | id)
look
check "image data" eq "$(field "$i1" 9)" 2x2x4
check "RGB, its stride's padding packed" eq "$(field "$i2" 9)" 2x2x3
check "image-data beats icon_data" eq "$(field "$i3" 9)" 1x1x4
check "refused images, still notified" eq "$(field "$i4" 9)$(field "$i5" 9)$(field "$i6" 9)$(field "$i7" 9)$(field "$i8" 9)" -----
check "wrong types ignored" eq "$(field "$i9" 9) $(field "$i9" 4) $(field "$i9" 6)" "- 1 -1"
check "image-path, app_icon" grep -q "^notif $p1 .* /tmp/a b.png dialog-information - path$" <<<"$state"
long=$(printf 'x%.0s' $(seq 2000))
l=$(notify app 0 "$long" '[]' '{}' 0)
look
check "the summary capped at 1 KiB" eq "$(field "$l" 13 | wc -c)" 1025

echo "-- reload"
before=$(count)
look restart
sleep 1
look
check "the plugins reloaded, the notifications stay" eq "$(count)" "$before"
r=$(notify app 0 after '[]' '{}' 0)
look
check "and new ones come" open_ "$r"

echo "-- caps"
# 50 at a time; not a bare wait, which would wait for the monitor too
for k in $(seq 300); do
	call Notify flood 0 '' "f$k" '' '[]' '{}' 'int32 0' >/dev/null &
	pids+=($!)
	[ $((k % 50)) = 0 ] && wait "${pids[@]}" && pids=()
done
look
check "256 open at most" grep -qx "notifs 256" <<<"$state"
check "critical ones stay" open_ "$c"
"$live" log | grep -q "notifications: .* closed (4)" || echo "      (no eviction logged)"

sleep 0.3
kill "$monitor" 2>/dev/null
check "signal: CloseNotification, reason 3" sawclosed "$b" 3
check "signal: expired, reason 1" sawclosed "$e" 1
check "signal: dismissed, reason 2" sawclosed "$a" 2
check "signal: ActionInvoked" sawaction "$x" reply
check "signal: ActivationToken" sawtoken "$x" tok123
check "signal: the action dismissed it" sawclosed "$x" 2
check "signal: the resident one's action" sawaction "$y" default
check "signal: evicted, reason 4" grep -q "NotificationClosed (uint32 [0-9]*, uint32 4)" "$signals"

"$live" alive || die "awl died"
out=$("$live" stop)
check "awl exits cleanly" grep -q "exited normally" <<<"$out"
echo "$fails failed"
exit "$fails"
