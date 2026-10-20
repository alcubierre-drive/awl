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
- calendar events: evolution-data-server (libecal), at run time only: the
  plugins load it if it is there, without it the calendar has no events
  (`-Decal_check=true` checks plugins/cal_eds.c against its headers)

awl builds with meson and ninja. scenefx is looked up under
`~/Desktop/awl-libs` (`awl_libs` in meson.options; empty uses pkg-config).
Then:

    meson setup build    # once
    ninja -C build       # after every change

This builds `build/awl`, `build/libawltray-env.so` (see `TRAY_ICONS` in
awl.h) and `build/libawlplugins.so`; awl finds both libraries next to itself.
`meson test -C build` runs the unit tests.

Switches (XWayland, the shatter effect, `-march=native`, `-ffast-math`,
scenefx's place) are in meson.options: `meson configure build` lists them,
`meson configure build -Dname=value` changes one. Flags of your own go into
meson.build's `custom_c_args`/`custom_link_args` (its `CUSTOM` part), or
`-Dc_args=...`/`-Dc_link_args=...` for one build directory.

## Configuration

The configuration is in two files:

- meson.options holds compile-time switches of the awl binary (its `CUSTOM`
  parts), so a change there needs a rebuild and a restart
- plugins_config.c is built into `libawlplugins.so`, so `ninja -C build` followed by
  `plugin_restart` (MOD+Ctrl+r) applies it to the running awl. Only its
  part between the `CUSTOM` markers is configuration: there,
  `configure(c, p)` gets awl's defaults (awl_config_defaults.h) in `c` and the
  plugins' (plugin_config_defaults.h) in `p`, and changes only what it wants,
  e.g. `c->modkey = WLR_MODIFIER_LOGO;`, `SETLIST(c->keys, keys);` for
  bindings of its own, or `p->colors.bg = ...;` for the bar; `awl_config_t`
  in awl.h and `awl_plugin_config_t` in plugins.h list everything there
  is.

The config's `lock` has the screen locker: `locker`, the ext-session-lock
client to run, `at_start`, and `on_suspend`, which makes suspend wait until
the locker covers every output (through a logind delay inhibitor). With
that, no separate lock handler (`xss-lock`, a `sleep.target` unit calling
`loginctl lock-session`) is needed; `loginctl lock-session` still works.

Its `CUSTOM` part covers keys, buttons, rules, layouts, tags, colors, font,
borders, blur, input devices, the keymap, the bar's place, the screen lock,
the programs started with awl, wlroots' log level, the alert, the popups,
the tray and wallpaper settings, and the bar's widgets and colors.

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
same but lets it read NetworkManager through `xdg-dbus-proxy`. A second
build directory,

    meson setup build-dev --buildtype=debug -Db_lto=false
    ninja -C build-dev

builds `build-dev/awl`, unoptimized with full debug info, for
`LIVE_AWL=build-dev/awl test/live.sh start`; `test/windows.sh` runs the
scenario test of awl's popups in such an instance, `meson test -C build` the
unit test of their drawing, which needs no compositor.

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
