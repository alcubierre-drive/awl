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
	# awl's alerts come whenever (the logind one a while after the start:
	# the test bus has no logind): dismissed, they don't count
	local a
	a=$(awk '$1 == "notif" && $(NF - 1) == "awl" && $NF == "alert" { print $2 }' <<<"$state")
	[ -z "$a" ] && return
	for a in $a; do "$live" win "nclose $a 2" >/dev/null; done
	# the windows follow in the next loop round
	state=$("$live" win) && [ -n "$state" ] || die "no state from awl"
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

# images: PNGs (a w x h square of color c), an icon theme and a desktop
# entry of their own, found first through $XDG_DATA_DIRS (the test home's
# icon folders are the user's)
png() {
	python3 -I -c 'import sys, zlib, struct
w, h, c, out = int(sys.argv[1]), int(sys.argv[2]), bytes.fromhex(sys.argv[3]), sys.argv[4]
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
raw = b"".join(b"\0" + c * w for _ in range(h))
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))' "$@"
}
share=$LIVE_DIR/share
img=$LIVE_DIR/img
rm -rf "$share" "$img"
mkdir -p "$share/icons/hicolor/48x48/apps" "$share/applications" "$img"
printf '[Icon Theme]\nName=Hicolor\nDirectories=48x48/apps\n\n[48x48/apps]\nSize=48\nType=Fixed\n' \
	>"$share/icons/hicolor/index.theme"
png 48 48 3366ccff "$share/icons/hicolor/48x48/apps/awltest-notif.png"
printf '[Desktop Entry]\nType=Application\nName=awl test\nExec=true\nIcon=awltest-notif\n' \
	>"$share/applications/awltest.desktop"
png 32 32 cc3333ff "$img/small.png"
png 32 32 33cc33ff "$img/a b.png"
# past icons' 1024 pixels a side, within a notification's limits
png 3000 2000 cccc33ff "$img/big.png"
mkfifo "$img/fifo"
# past a notification's 4096 x 3072
png 5000 3000 cccc33ff "$img/huge.png"
export XDG_DATA_DIRS=$share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}

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

echo "-- windows (plugins/notify.c)"
# the visible notification windows (the plugins' persistent ones), top to
# bottom: "I X Y W H"; the "+k more" one among them
nwins() { awk '$1 == "pwin" && $8 == 1 && $9 == 1 { print $2, $4, $5, $6, $7 }' <<<"$state" | sort -k3 -n; }
nwincount() { nwins | wc -l; }
# the center of part P of the K-th window from the top: "X Y"
partat() {
	local i
	i=$(nwins | sed -n "$1p" | cut -d' ' -f1)
	[ -n "$i" ] && awk -v i="$i" -v p="$2" '$1 == "pcell" && $2 == i && $3 == p { print $4, $5 }' <<<"$state"
}
# the K-th window from the top: "X Y W H"
boxof() { nwins | sed -n "$1p" | cut -d' ' -f2-; }
press() { "$live" click "$1" "$2" "$3" >/dev/null || die "click"; sleep 0.3; }
notifs() { awk '$1 == "notifs" { print $2 }' <<<"$state"; }
read -r px py < <(partat 1 0)
[ -n "${px:-}" ] || die "no notification window"
press "$px" "$py" middle
look
check "a middle click dismisses all" eq "$(notifs) $(nwincount)" "0 0"
check "signal: dismissed, the critical one too" sawclosed "$c" 2
w1=$(notify app 0 plain '[]' '{}' 0)
w2=$(notify app 0 act '["default", "Open", "reply", "Reply"]' '{}' 0)
look
check "a window each" eq "$(nwincount)" 2
check "newest on top: the one with a button" eval '[ -n "$(partat 1 1)" ] && [ -z "$(partat 2 1)" ]'
read -r x1 y1 w1_ h1 < <(boxof 1)
read -r x2 y2 w2_ h2 < <(boxof 2)
check "stacked, right edges aligned, not overlapping" eval '[ $((x1 + w1_)) = $((x2 + w2_)) ] && [ "$y2" -gt $((y1 + h1)) ]'
read -r px py < <(partat 1 1)
press "$px" "$py" left
look
check "a button invokes its action" sawaction "$w2" reply
check "and closes it" closed "$w2"
w3=$(notify app 0 def '["default", "Open"]' '{}' 0)
look
read -r px py < <(partat 1 0)
press "$px" "$py" left
look
check "a click on it: the default action" sawaction "$w3" default
read -r px py < <(partat 1 0)
press "$px" "$py" right
look
check "a right click dismisses" eval 'closed "$w1" && sawclosed "$w1" 2'
h=$(notify app 0 hold '[]' '{}' 1000)
look
read -r px py < <(partat 1 0)
"$live" click "$px" "$py" >/dev/null
sleep 1.5
look
check "the pointer on it holds it" open_ "$h"
"$live" click 300 300 >/dev/null
sleep 1.5
look
check "off it, it expires" closed "$h"
v=$(notify vol 0 vol '[]' '{"value": <byte 10>}' 0)
look
b1=$(boxof 1)
notify vol "$v" vol '[]' '{"value": <byte 90>}' 0 >/dev/null
look
check "a new value: redrawn in place" eq "$(boxof 1)" "$b1"
# the bottom of the room the windows have: the window area less the margin
read -r wy wh < <(awk '$1 == "mon" { print $8, $10; exit }' <<<"$state")
mg=$(awk '$1 == "margin" { print $2 }' <<<"$state")
[ "$mg" -gt $((wh / 4)) ] && mg=$((wh / 4))
bottom=$((wy + wh - mg))
# all visible windows inside it, none overlapping the one above
inside() {
	nwins | awk -v b="$bottom" '{ if ($3 + $5 > b || (NR > 1 && $3 < last)) bad = 1; last = $3 + $5 } END { exit bad }'
}
for k in $(seq 15); do notify app 0 "m$k" '[]' '{}' 0 >/dev/null; done
look
shown=$(nwincount)
check "not all shown, \"+k more\" under them, all inside" eval 'inside && [ "$shown" -lt "$(notifs)" ] && [ "$shown" -gt 2 ]'
look restart
sleep 1
look
check "after a reload: the windows are back" eq "$(nwincount)" "$shown"
read -r px py < <(partat 1 0)
press "$px" "$py" middle
look
check "and work" eq "$(notifs) $(nwincount)" "0 0"

echo "-- awl's own (awl_notifications_send())"
# nsend URGENCY TIMEOUT LINES WIDTH TAG SUMMARY BODY: awl's own, then a
# loop round for the library's windows
nsend() {
	look "nsend $1 $2 $3 $4 $5 $6"$'\\t'"$7"
	look
}
newest() { grep '^notif ' <<<"$state" | head -1 | cut -d' ' -f2; }
nsend 1 0 0 0 - own body
o=$(newest)
check "it comes, flagged AWL_NOTIF_AWL" eval '[ "$(field "$o" 13)" = own ] && [ $(($(field "$o" 7) & 4)) = 4 ]'
check "a window for it" eq "$(nwincount)" 1
call CloseNotification "uint32 $o" >/dev/null
look
check "CloseNotification doesn't close it" open_ "$o"
g=$(field "$o" 3)
r=$(notify app "$o" taken '[]' '{}' 0)
look
check "replaces_id doesn't replace it" eval '[ "$r" != "$o" ] && [ "$(field "$o" 3)" = "$g" ]'
nsend 1 0 0 0 tag tagged one
t1=$(newest)
nsend 1 0 0 0 tag tagged two
t2=$(newest)
check "a tag replaces awl's own in place" eval '[ "$t1" = "$t2" ] && [ "$(count)" = 3 ]'
look "nclose $o 2"
look
check "the library closes it" closed "$o"
read -r px py < <(partat 1 0)
press "$px" "$py" middle
look
check "all dismissed" eq "$(notifs) $(nwincount)" "0 0"
long=$(printf 'line %d\\n' $(seq 40))
nsend 1 0 6 0 - six "$long"
nsend 1 0 2 0 - two "$long"
nsend 1 0 2 200 - narrow "$long"
read -r _ _ wn hn < <(boxof 1)
read -r _ _ w2_ h2 < <(boxof 2)
read -r _ _ w6 h6 < <(boxof 3)
check "its own max_lines: lower" eval '[ "$h2" -lt "$h6" ] && [ "$hn" = "$h2" ]'
check "its own width: narrower" eval '[ "$wn" -lt "$w2_" ] && [ "$w2_" = "$w6" ]'
read -r px py < <(partat 1 0)
press "$px" "$py" middle

echo "-- the room (plugins/notify.c)"
nsend 1 0 -1 0 - huge "$long"
check "one higher than the room: cut to it" eval 'inside && [ "$(nwincount)" = 1 ]'
read -r _ yc _ hc < <(boxof 1)
check "to what is left of it" eval '[ $((bottom - yc - hc)) -lt 30 ]'
for k in 1 2 3; do nsend 1 0 -1 0 - "huge$k" "$long"; done
check "four of them: the newest cut, \"+3 more\" under it" eval 'inside && [ "$(notifs) $(nwincount)" = "4 2" ]'
read -r px py < <(partat 1 0)
press "$px" "$py" middle
for k in $(seq 4); do nsend 1 0 6 0 - "short$k" "$long"; done
check "what fits is whole" eval 'inside && [ "$(boxof 1 | cut -d" " -f4) $(boxof 2 | cut -d" " -f4)" = "$h6 $h6" ]'
read -r px py < <(partat 1 0)
press "$px" "$py" middle
look
check "awl's own: no signal" eval '! grep -q "NotificationClosed (uint32 $o," "$signals"'

echo "-- images (plugins/notify_image.c)"
# ICON HINTS: a notification shown alone, its window's height in $hof
# once it showed (up to 2 s), the first height seen in $first; closed
# after
one() {
	local i k
	first= hof=
	i=$(call Notify app 0 "$1" summary '' '[]' "$2" 'int32 0' | id)
	for k in $(seq 20); do
		look
		hof=$(boxof 1 | cut -d' ' -f4)
		[ -z "$first" ] && first=$hof
		[ -n "$hof" ] && [ "$k" -gt 3 ] && break
		sleep 0.1
	done
	call CloseNotification "uint32 $i" >/dev/null
}
one '' '{}'
h0=$hof
taller() { [ -n "$hof" ] && [ "$hof" -gt "$h0" ]; }
plain() { [ "$hof" = "$h0" ]; }
one '' "{\"image-path\": <\"$img/small.png\">}"
check "image-path, a file: the image" taller
check "shown with it from the start" eq "$first" "$hof"
one '' "{\"image-path\": <\"file://$img/a%20b.png\">}"
check "a file:// URL, percent-decoded" taller
one "$img/small.png" '{}'
check "app_icon, a file" taller
one awltest-notif '{}'
check "app_icon, an icon name" taller
one '' '{"desktop-entry": <"awltest">}'
check "desktop-entry's Icon" taller
one "$img/small.png" "{\"image-path\": <\"$img/none.png\">}"
check "image-path missing: app_icon" taller
one '' "{\"image-path\": <\"$img/big.png\">}"
check "a 3000x2000 PNG" taller
one '' "{\"image-path\": <\"$img/none.png\">}"
check "missing: none" plain
one '' '{"image-path": <"/dev/zero">}'
check "no regular file: none" plain
one '' "{\"image-path\": <\"$img/fifo\">}"
check "a FIFO: none, not stuck on it" plain
one '' "{\"image-path\": <\"$img/small.png\">}"
check "and the next one comes" taller
check "the second time from the cache" eval '"$live" log | grep -q "notify: image $img/small.png at [0-9]* from the cache"'
# a changed file is decoded again: now it is none
printf 'not a PNG' >"$img/small.png.new" && mv "$img/small.png.new" "$img/small.png"
one '' "{\"image-path\": <\"$img/small.png\">}"
check "a changed file: decoded again" plain
png 32 32 cc3333ff "$img/small.png"
one '' "{\"image-path\": <\"$img/small.png\">}"
check "and changed back" taller
one '' "{\"image-path\": <\"$img/huge.png\">}"
check "past 12 megapixels: none" plain
one '' '{"desktop-entry": <"../applications/awltest">}'
check "a desktop entry id with a slash: none" plain
i=$(call Notify app 0 awltest-notif reload '' '[]' '{}' 'int32 0' | id)
look restart
sleep 1
look
check "after a reload: the image again" eval '[ "$(boxof 1 | cut -d" " -f4)" -gt "$h0" ]'
call CloseNotification "uint32 $i" >/dev/null

echo "-- the menu, do-not-disturb, the widget (plugins/notify_menu.c)"
# the menu: the visible popup that isn't persistent; its row P's center
menu() { awk '$1 == "pwin" && $8 == 1 && $9 == 0 { print $2; exit }' <<<"$state"; }
menurow() {
	local i
	i=$(menu)
	[ -n "$i" ] && awk -v i="$i" -v p="$1" '$1 == "pcell" && $2 == i && $3 == p { print $4, $5 }' <<<"$state"
}
menurows() { local i; i=$(menu); [ -n "$i" ] && awk -v i="$i" '$1 == "pcell" && $2 == i' <<<"$state" | wc -l; }
# the notifications' widget: the defaults' second from the right
widget() { awk '$1 == "widget" && $3 == 1 { print $4, $5; exit }' <<<"$state"; }
clickrow() { local x y; read -r x y < <(menurow "$1"); [ -n "${x:-}" ] && press "$x" "$y" "$2"; look; }
clickwidget() { local x y; read -r x y < <(widget); [ -n "${x:-}" ] && press "$x" "$y" "$1"; look; }
look
read -r wx wy < <(widget)
check "the widget is on the bar" eval '[ -n "${wx:-}" ]'
m1=$(notify app 0 one '[]' '{}' 0)
m2=$(notify app 0 two '["default", "Open", "reply", "Reply"]' '{}' 0)
m3=$(notify app 0 three '[]' '{"urgency": <byte 2>}' 0)
look
clickwidget left
check "a left click on it opens the menu" eval '[ -n "$(menu)" ]'
check "which hides the windows" eq "$(nwincount)" 0
# do-not-disturb, dismiss all, three, two, Reply, one
check "a row each: the switch, dismiss all, the titles, the action" eq "$(menurows)" 6
clickrow 4 left
check "an action's row invokes it" sawaction "$m2" reply
check "and closes the menu" eval '[ -z "$(menu)" ]'
clickwidget left
clickrow 3 right
check "a right click on a title dismisses it" eval 'closed "$m1" && sawclosed "$m1" 2'
check "the menu stays, without it" eq "$(menurows)" 3
clickrow 0 left
check "do-not-disturb from the menu" eval '"$live" log | grep -q "notify: do-not-disturb on"'
m4=$(notify app 0 four '[]' '{}' 0)
look
check "the menu lists the new one too" eq "$(menurows)" 4
clickwidget left
check "closed: the windows again; while on, only the critical one" eq "$(nwincount)" 1
look restart
sleep 1
look
check "after a reload: still on" eq "$(nwincount)" 1
clickwidget right
check "a right click on the widget: off again" eq "$(nwincount)" 2
clickwidget middle
check "a middle click on it dismisses all" eq "$(notifs) $(nwincount)" "0 0"
for k in $(seq 15); do notify app 0 "k$k" '[]' '{}' 0 >/dev/null; done
look
n=$(nwincount)
read -r px1 py1 pw1 _ < <(boxof 1)
read -r px py < <(partat "$n" 0)
press "$px" "$py" left
look
check "a click on \"+k more\" opens the menu" eval '[ -n "$(menu)" ]'
read -r mx my mw _ < <(awk -v i="$(menu)" '$1 == "pwin" && $2 == i { print $4, $5, $6, $7 }' <<<"$state")
check "in the stack's place, its corner" eq "$((mx + mw)) $my $(nwincount)" "$((px1 + pw1)) $py1 0"
clickrow 1 left
check "dismiss all from the menu" eq "$(notifs) $(nwincount)" "0 0"
check "an empty menu: the switch and \"No notifications\"" eq "$(menurows)" 2

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
