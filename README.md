# awl

awl is a Wayland compositor built on [wlroots] and [scenefx]. It started as a
fork of [dwl], the dwm-like compositor, and has grown into a small desktop of
its own:

- a built-in bar with tags, a task bar, the layout, and status widgets (CPU,
  memory and swap graphs, temperature, battery, volume, IP address, clock)
- a system tray (StatusNotifierItem)
- a calendar popup on the clock that shows evolution-data-server events
- a wallpaper with timed changes and fades
- a panel listing the files in `~/Desktop`
- blur and rounded corners for layer surfaces (scenefx)
- desktop notifications for its own events
- screen locking with an ext-session-lock client (swaylock by default) on
  `loginctl lock-session` and before suspend, which waits for the lock; a red
  alert on screen if that stops working

Most of that lives in `libawlplugins.so`, which a running awl rebuilds and
reloads without a restart.

## Building

Dependencies:

- awl: wlroots 0.20, scenefx 0.5, wayland, xkbcommon, libinput, pixman,
  fcft, libpulse, libsystemd, and xcb and xcb-icccm for XWayland
- the tray (`tray/`): gtkmm 3, gtk-layer-shell (gdk-pixbuf, for the menus' icons, comes with them)
- calendar events: evolution-data-server (libecal), at run time only: the
  plugins load it if it is there, without it the calendar has no events
  (`make ECAL_CHECK=1` checks plugins/cal_eds.c against its headers)

scenefx is looked up under `~/Desktop/awl-libs` (`AWL_LIBS` in config.mk).
Then:

    make

This builds `awl`, `tray/libawltray.so` and `libawlplugins.so`. On the first
build, config_awl.h and config_plugins.h are copied from their .def.h.

## Configuration

The configuration is in two files:

- config_awl.h is read once at startup, so a change there needs a restart
- config_plugins.h is built into `libawlplugins.so` too, so `make` followed by
  `plugin_restart` (MOD+Ctrl+r) applies it to the running awl

config_awl.h has the screen locker: `locker`, the ext-session-lock client to
run, `lock_at_start`, and `lock_on_suspend`, which makes suspend wait until
the locker covers every output (through a logind delay inhibitor). With
that, no separate lock handler (`xss-lock`, a `sleep.target` unit calling
`loginctl lock-session`) is needed; `loginctl lock-session` still works.

config_plugins.h covers keys, buttons, rules, layouts, colors, font, borders,
blur, input devices, the keymap, and the tray and wallpaper settings.

## Running

    awl [-s startup command] [-l logfile] [-d] [-v]

`-s` runs a shell command at startup. awl sends it SIGTERM when it exits.
`-d` turns on debug logging (wlroots' and awl's verbose messages). awl logs to stderr, colored on a terminal, and uncolored to the file given with `-l`, by default `/tmp/awl.log` (`-DAWL_LOGFILE=...` changes the default); `-l ''` logs to stderr only. The file starts over on each start, and once it passes 128 KiB.

Like dwl, awl runs on any wlroots backend: nested in an X11 or Wayland
session, or directly on a VT. On a VT you need a seat, either from
logind with polkit, or from seatd. To start it from a display
manager, copy `awl.desktop` to `/usr/share/wayland-sessions/`.

`test/live.sh` runs a headless instance for testing, see the comment at its
top. It keeps the instance off the real system bus; `test/live-nm.sh` does the
same but lets it read NetworkManager through `xdg-dbus-proxy`. `make dev`
builds `dev/awl`, unoptimized with full debug info, for
`LIVE_AWL=dev/awl test/live.sh start`; `test/windows.sh` runs the scenario
test of awl's popups in such an instance, `make check` the unit test of their
drawing, which needs no compositor.

## Documentation

The documentation covers the architecture, live reload and the threads, plus
an API reference generated from the sources:

    make -C doc venv    # once
    make -C doc

Then open `doc/_build/html/index.html`.

## License

GPLv3 or later, see LICENSE. That file also carries the licenses of the code
awl inherited from dwl, dwm, sway and drwl.

## Acknowledgements

awl would not exist without [dwl] and its contributors, and through dwl
without [dwm], [TinyWL] and [sway]. The bar's drawing code started out as
[drwl].

[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots/
[scenefx]: https://github.com/wlrfx/scenefx
[dwl]: https://codeberg.org/dwl/dwl
[dwm]: https://dwm.suckless.org/
[TinyWL]: https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/master/tinywl
[sway]: https://github.com/swaywm/sway
[drwl]: https://codeberg.org/sewn/drwl
