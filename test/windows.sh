#!/bin/bash
# Scenario test of awl's popups (awl_window.c, awl_popup.c): a
# live.sh test instance whose popups it drives through AWL_TEST_WINDOWS (see
# testwincmd() in awl_test.c), checking after every step where they are -- inside
# their monitor and against its window area's edge, hidden once the monitor
# is gone or off, none after a plugin reload -- that awl is still alive, and
# at the end that it exits cleanly, popups still open.
#
#   test/windows.sh     with dev/awl (make dev) if it is there, else awl
#
# LIVE_AWL picks the binary, LIVE_NESTED=1 shows the outputs as windows (see
# live.sh). The state goes to $LIVE_DIR, by default /tmp/awl-windows-$UID, so
# a live.sh instance of your own can keep running. Exit status: the number
# of failed checks (100: awl died, the rest was skipped).
set -u
here=$(cd "$(dirname "$0")" && pwd)
live=$here/live.sh
export LIVE_DIR=${LIVE_DIR:-/tmp/awl-windows-$UID}
[ -x "$here/../dev/awl" ] && export LIVE_AWL=${LIVE_AWL:-dev/awl}
fails=0
state=

die() {
	echo "FATAL $*, log: $LIVE_DIR/awl.log"
	"$live" log | tail -40
	"$live" stop >/dev/null 2>&1
	exit 100
}

# a command for the popups (live.sh win); state is what they look like after
win() {
	"$live" alive || die "awl died"
	state=$("$live" win "$@") && [ -n "$state" ] || die "no state from awl"
}

# check DESCRIPTION TEST [ARGS...]: TEST runs on $state
check() {
	local d=$1
	shift
	if "$@"; then
		echo "ok    $d"
	else
		echo "FAIL  $d"
		sed 's/^/        /' <<<"$state"
		fails=$((fails + 1))
	fi
}

# window N visible on MON, inside it, the margin (as much as fits, see
# awl_window_place()) away from its sides and its window area's top or
# bottom; N "any" with KIND pwin: some window of the plugins on MON
shown() {
	awk -v n="$1" -v mon="$2" -v kind="${3:-win}" -v m="$(awk '$1 == "margin" { print $2 }' <<<"$state")" '
	function gap(m, e) { m = m < 0 ? 0 : m; return m < int(e / 4) ? m : int(e / 4) }
	$1 == "mon" { x[$2] = $3; y[$2] = $4; w[$2] = $5; h[$2] = $6; wy[$2] = $8; wh[$2] = $10 }
	$1 == kind && ($2 == n || n == "any" && $3 == mon) && !ok {
		found = 1
		mx = gap(m, w[mon]); if (mx > int((w[mon] - $6) / 2)) mx = $6 < w[mon] ? int((w[mon] - $6) / 2) : 0
		my = gap(m, wh[mon]); if (my > wh[mon] - $7) my = $7 < wh[mon] ? wh[mon] - $7 : 0
		ok = $3 == mon && $8 == 1 && $6 > 0 && $7 > 0 &&
			$4 >= x[mon] + mx && $4 + $6 <= x[mon] + w[mon] - mx &&
			$5 >= y[mon] && $5 + $7 <= y[mon] + h[mon] &&
			($5 == wy[mon] + my || $5 + $7 == wy[mon] + wh[mon] - my)
	}
	END { exit !(found && ok) }' <<<"$state"
}
# window N exists, but is hidden
hidden() { awk -v n="$1" '$1 == "win" && $2 == n { f = 1; ok = $3 == "-" && $8 == 0 } END { exit !(f && ok) }' <<<"$state"; }
# window N is still shown on MON, but invisible (MON is off)
parked() { awk -v n="$1" -v mon="$2" '$1 == "win" && $2 == n { f = 1; ok = $3 == mon && $8 == 0 } END { exit !(f && ok) }' <<<"$state"; }
# window N does not exist (destroyed)
gone() { ! grep -q "^win $1 " <<<"$state"; }
nowindows() { ! grep -q '^win ' <<<"$state"; }
noerrors() { grep -qx 'errors 0' <<<"$state"; }
# column C (1-based, as in the dump) of window N
field() { awk -v n="$1" -v c="$2" '$1 == "win" && $2 == n { print $c }' <<<"$state"; }
# none of the plugins' windows is shown on MON
pnoneon() { ! awk -v mon="$1" '$1 == "pwin" && $3 == mon { v = 1 } END { exit !v }' <<<"$state"; }
# none of the plugins' windows is visible
pnone() { ! awk '$1 == "pwin" && $8 == 1 { v = 1 } END { exit !v }' <<<"$state"; }
# where to point at MON's first bar widget with a hover popup: "X Y"
hoverat() { awk -v mon="$1" '$1 == "hover" && $2 == mon { print $3, $4; exit }' <<<"$state"; }
# the middle of the gap between the plugins' first visible window on MON
# and its window area's edge: "X Y"
pgap() {
	awk -v mon="$1" '
	$1 == "mon" && $2 == mon { wy = $8; wh = $10 }
	$1 == "pwin" && $3 == mon && $8 == 1 {
		x = int($4 + $6 / 2)
		print x, $5 - wy < wy + wh - ($5 + $7) ? int((wy + $5) / 2) : int(($5 + $7 + wy + wh) / 2)
		exit
	}' <<<"$state"
}
# the center of the plugins' first visible window on MON: "X Y"
pcenter() { awk -v mon="$1" '$1 == "pwin" && $3 == mon && $8 == 1 { print int($4 + $6 / 2), int($5 + $7 / 2); exit }' <<<"$state"; }
# move the pointer to X Y (no button) and wait out hover_delay_ms
point() { "$live" click "$1" "$2" >/dev/null || die "pointer"; sleep 0.4; }
# the center of window N's cell ROW COL: "X Y"
cellat() { awk -v n="$1" -v r="$2" -v c="$3" '$1 == "cell" && $2 == n && $3 == r && $4 == c { print $5, $6 }' <<<"$state"; }
# the press since the last dump: "N ROW COL BUTTON" (BTN_LEFT 272, BTN_RIGHT 273)
clicked() { grep -qx "click $1" <<<"$state"; }
noclicks() { ! grep -q '^click ' <<<"$state"; }
# a scroll since the last dump: "N ROW COL ORIENTATION VALUE120 SIGN"
# (orientation 0 vertical, 1 horizontal; SIGN the delta's)
scrolled() { grep -qx "scroll $1" <<<"$state"; }
noscrolls() { ! grep -q '^scroll ' <<<"$state"; }
# window N's highlighted row
hoverrow() { test "$(field "$1" 9)" = "$2"; }
# the newest monitor's name: createmon() puts it first
newest() { awk '$1 == "mon" { print $2; exit }' <<<"$state"; }
randr() { "$live" randr "$@" >/dev/null || die "wlr-randr $*"; sleep 0.3; }

"$live" alive && die "a test instance already runs in $LIVE_DIR"
LIVE_OUTPUTS=1 "$live" start >/dev/null || die "no start"
M1=HEADLESS-1
table='cpu\t42 °C\t1.2 GHz\ngpu\t51 °C\t0.8 GHz\nnvme\t38 °C'

echo "-- one output"
win show 0 $M1 640 "$table"
check "shown" shown 0 $M1
w0=$(field 0 6)
win update 0 "$table"'\nchipset with a long name\t60 °C\t-'
check "update: still shown" shown 0 $M1
check "update: wider" test "$(field 0 6)" -gt "$w0"
for x in 0 1279 -500 100000; do
	win show 0 $M1 $x "$table"
	check "centered on $x: kept inside" shown 0 $M1
done
win hide 0
check "hide" hidden 0
win update 0 "while hidden"
check "update while hidden: stays hidden" hidden 0
win toggle 0 $M1 640 "$table"
check "toggle hidden: shown" shown 0 $M1
win toggle 0 $M1 640 "$table"
check "toggle shown: hidden" hidden 0

echo "-- odd text"
win show 0 $M1 640 ""
check "empty text: hidden or shown inside" eval 'shown 0 $M1 || ! awk '\''$1 == "win" && $2 == 0 && $8 == 1 { v = 1 } END { exit !v }'\'' <<<"$state"'
win show 0 $M1 640 '\t\t\n\n\t'
check "only separators: hidden or shown inside" eval 'shown 0 $M1 || ! awk '\''$1 == "win" && $2 == 0 && $8 == 1 { v = 1 } END { exit !v }'\'' <<<"$state"'
win show 0 $M1 640 "$(printf 'c%d\\t' $(seq 30))"
check "30 columns (16 max)" shown 0 $M1
win show 0 $M1 640 "<b>&amp;</b> \\\\ ü € 😀"
check "markup characters, UTF-8, emoji" shown 0 $M1
win show 0 $M1 640 "$(printf 'x%.0s' $(seq 400))"
check "wider than the monitor: kept inside" shown 0 $M1
win show 0 $M1 640 "$(printf 'row %d\\n' $(seq 100))"
check "taller than the window area: rows cut, kept inside" shown 0 $M1

echo "-- outputs"
"$live" add
win
M2=$(newest)
check "a second output" test "$M2" != $M1
win show 0 $M1 640 "$table"
win show 1 "$M2" 640 "on the second output"
check "window 1 on $M2" shown 1 "$M2"
check "window 0 still on $M1" shown 0 $M1
randr --output "$M2" --off
win
check "$M2 off: window 1 invisible" parked 1 "$M2"
randr --output "$M2" --on
win
check "$M2 on again: window 1 back" shown 1 "$M2"
win show 0 "$M2" 100 "moved over"
check "window 0 moved to $M2" shown 0 "$M2"
"$live" remove
win
check "$M2 removed: window 0 hidden" hidden 0
check "$M2 removed: window 1 hidden" hidden 1
win show 1 $M1 640 "back on $M1"
check "window 1 shown again on $M1" shown 1 $M1
win update 0 "update after its monitor was removed"
check "update of a window whose monitor was removed: stays hidden" hidden 0

echo "-- scale"
for s in 1.5 1.25 2 1; do
	randr --output $M1 --scale $s
	win
	check "scale $s: window 1 inside" shown 1 $M1
done

echo "-- input"
win show 0 $M1 640 "$table"
win show 1 $M1 100 "second"
"$live" click 640 300 left >/dev/null || die "click"
sleep 0.2
win
check "a click outside hides window 0" hidden 0
check "a click outside hides window 1" hidden 1

echo "-- pointer input"
win show 0 $M1 640 "$table"
win show 1 $M1 100 "no input"
win input 0 on
check "input on: still shown" shown 0 $M1
read -r cx cy < <(cellat 0 1 2)
[ -n "${cx:-}" ] || die "no cell 1 2 in the dump"
point "$cx" "$cy"
win
check "the pointer on row 1 highlights it" hoverrow 0 1
"$live" click "$cx" "$cy" left >/dev/null
win
check "a click on cell 1, 2 reaches it" clicked "0 1 2 272"
check "a click on it doesn't close it" shown 0 $M1
check "nor the other popups" shown 1 $M1
read -r bx by < <(awk '$1 == "win" && $2 == 0 { print $4 + 2, int($5 + $7 / 2) }' <<<"$state")
"$live" click "$bx" "$by" right >/dev/null
win
check "a right click on the padding: no cell" clicked "0 -1 -1 273"
check "the padding is no row" hoverrow 0 -1
read -r cx cy < <(cellat 0 2 0)
point "$cx" "$cy"
win
check "row 2" hoverrow 0 2
point 300 300
win
check "leaving it: no row" hoverrow 0 -1
check "nothing recorded without a click" noclicks
read -r ox oy < <(awk '$1 == "win" && $2 == 1 { print int($4 + $6 / 2), int($5 + $7 / 2) }' <<<"$state")
"$live" click "$ox" "$oy" left >/dev/null
win
check "a click on a popup without input goes through: it closes" hidden 1
check "and so does the one with input" hidden 0
check "which got nothing" noclicks
win show 0 $M1 640 "$table"
read -r cx cy < <(cellat 0 0 0)
point "$cx" "$cy"
win destroy 0
point 300 300
point "$cx" "$cy"
win
check "destroying the window under the pointer" gone 0
win show 0 $M1 640 "$table"
win input 0 off
win input 0 on
win input 0 off
read -r cx cy < <(cellat 0 0 0)
"$live" click "$cx" "$cy" left >/dev/null
win
check "input off: clicks go through again" hidden 0
check "input off: nothing recorded" noclicks
"$live" add
win
M2=$(newest)
win show 0 "$M2" 640 "$table"
win input 0 on
read -r cx cy < <(cellat 0 1 0)
point "$cx" "$cy"
win
check "on $M2: row 1" hoverrow 0 1
"$live" remove
point 300 300
win
check "$M2 removed under the pointer: hidden, no row" eval 'hidden 0 && hoverrow 0 -1'
win destroy 0
win destroy 1

echo "-- scroll"
win show 0 $M1 640 "$table"
win show 1 $M1 100 "no input"
win input 0 on
read -r cx cy < <(cellat 0 1 0)
[ -n "${cx:-}" ] || die "no cell 1 0 in the dump"
"$live" click "$cx" "$cy" wheel:1 >/dev/null
win
check "a scroll on a popup with only a click callback: dropped" eval 'noscrolls && noclicks'
check "and it stays open" shown 0 $M1
win scroll 0 on
"$live" click "$cx" "$cy" wheel:1 >/dev/null
win
check "a notch down on row 1 reaches it" scrolled "0 1 0 0 120 1"
"$live" click "$cx" "$cy" wheel:-2 hwheel:1 >/dev/null
win
check "two notches up" scrolled "0 1 0 0 -240 -1"
check "a notch right" scrolled "0 1 0 1 120 1"
"$live" click "$cx" "$cy" finger:5 >/dev/null
win
check "a touchpad's scroll: no notches" scrolled "0 1 0 0 0 1"
read -r bx by < <(awk '$1 == "win" && $2 == 0 { print $4 + 2, int($5 + $7 / 2) }' <<<"$state")
"$live" click "$bx" "$by" wheel:1 >/dev/null
win
check "a scroll on the padding: no cell" scrolled "0 -1 -1 0 120 1"
check "nor a click" noclicks
check "scrolling closes no popup" eval 'shown 0 $M1 && shown 1 $M1'
win input 0 off
read -r cx cy < <(cellat 0 2 1)
"$live" click "$cx" "$cy" wheel:1 >/dev/null
win
check "scroll only: still reaches it" scrolled "0 2 1 0 120 1"
"$live" click "$cx" "$cy" left >/dev/null
win
check "scroll only: a click on it is dropped, nothing closes" eval 'shown 0 $M1 && shown 1 $M1 && noclicks'
read -r ox oy < <(awk '$1 == "win" && $2 == 1 { print int($4 + $6 / 2), int($5 + $7 / 2) }' <<<"$state")
"$live" click "$ox" "$oy" wheel:1 >/dev/null
win
check "a scroll on a popup without input goes through: nothing recorded" noscrolls
check "and it stays open" shown 1 $M1
win scroll 0 off
"$live" click "$cx" "$cy" wheel:1 >/dev/null
win
check "scroll off: scrolling goes through again" noscrolls
"$live" click "$cx" "$cy" left >/dev/null
win
check "scroll off: so do clicks, it closes" hidden 0
win show 0 $M1 640 "$table"
win scroll 0 on
read -r cx cy < <(cellat 0 0 0)
point "$cx" "$cy"
win destroy 0
"$live" click "$cx" "$cy" wheel:1 >/dev/null
win
check "a scroll where a destroyed window was" gone 0
win destroy 1

echo "-- hover (a widget's popup, through the plugin API)"
"$live" click 300 300 >/dev/null
win
read -r hx hy < <(hoverat $M1)
if [ -z "${hx:-}" ]; then
	echo "skip  no bar widget with an awl_window hover popup"
else
	point "$hx" "$hy"
	win
	check "hover opens the widget's popup" shown any $M1 pwin
	point 300 300
	win
	check "leaving closes it" pnone
	point "$hx" "$hy"
	win
	read -r gx gy < <(pgap $M1)
	read -r px py < <(pcenter $M1)
	point "$gx" "$gy"
	win
	check "the pointer in the gap to the bar keeps it open" shown any $M1 pwin
	point "$px" "$py"
	win
	check "the pointer in the popup keeps it open" shown any $M1 pwin
	point 300 300
	win
	check "leaving the popup closes it" pnone
	# hover only (awl_bar.c's barbutton()): a press on the widget closes its
	# popup, and it stays closed until the pointer leaves and comes back
	point "$hx" "$hy"
	"$live" click "$hx" "$hy" left >/dev/null
	sleep 0.2
	win
	check "a click on the widget closes the hover's popup" pnone
	"$live" click "$hx" "$hy" left >/dev/null
	sleep 0.3
	win
	check "another click doesn't open it" pnone
	point 300 300
	point "$hx" "$hy"
	win
	check "back on the widget: its hover opens it again" shown any $M1 pwin
	point 300 300
	"$live" click 300 300 left >/dev/null
	sleep 0.2
	win
	check "off the widget, a click elsewhere: closed" pnone
	point "$hx" "$hy"
	randr --output $M1 --scale 1.5
	win
	read -r hx hy < <(hoverat $M1)
	# updatebar() scales the font by size and dpi, i.e. by scale squared: at
	# 1.5 the right-hand widgets may not fit, and end up past the edge
	if [ "$hx" -lt "$(awk -v m=$M1 '$1 == "mon" && $2 == m { print $3 + $5 }' <<<"$state")" ]; then
		point "$hx" "$hy"
		win
		check "scale 1.5: popup inside" shown any $M1 pwin
	else
		echo "skip  scale 1.5: the widget is off the bar (x $hx)"
	fi
	randr --output $M1 --scale 1
	point 300 300
	"$live" add
	win
	M2=$(newest)
	read -r hx2 hy2 < <(hoverat "$M2")
	point "$hx2" "$hy2"
	win
	check "hover on $M2" shown any "$M2" pwin
	"$live" remove
	win
	# the pointer lands on $M1's bar, maybe on the same widget there (the
	# clock), whose hover may open the popup again on $M1: nothing may be
	# left on $M2
	check "$M2 removed while its popup was open" pnoneon "$M2"
	point 300 300
	win restart
	sleep 1
	win
	read -r hx hy < <(hoverat $M1)
	point "$hx" "$hy"
	win
	check "hover after a plugin reload" shown any $M1 pwin
	point 300 300
fi

echo "-- lifetime"
win destroy 1
check "destroy" gone 1
win update 1 "x"
win hide 1
win destroy 1
check "update, hide, destroy of a destroyed window: nothing" gone 1
win show 0 $M1 640 "$table"
win show 1 $M1 100 "second"
win restart
sleep 1
win
check "plugin reload destroys them" nowindows
win show 0 $M1 640 "after the reload"
check "new window after the reload" shown 0 $M1
check "no rejected commands" noerrors

echo "-- exit"
win show 1 $M1 100 "open at exit"
out=$("$live" stop)
state=$out
check "clean exit with popups open" grep -q "exited normally" <<<"$out"

echo "$fails failed, log: $LIVE_DIR/awl.log"
exit $fails
