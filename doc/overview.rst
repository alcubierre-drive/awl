Architecture
============

awl is two binaries:

``awl``
   The compositor: wlroots (with scenefx for blur and rounded corners), the
   windows, the input, the layouts, the bar's buffers and its layout, the
   wallpaper's and the desktop panel's scene nodes. Its main thread runs the
   Wayland event loop and is the only one that touches wlroots.

``libawlplugins.so``
   Everything that is meant to change often: the plugin threads that collect
   what the bar shows, the bar widgets (the tray among them, see
   :doc:`api/tray`), the desktop panel's contents, picking
   and decoding the wallpaper, and plugins_config.c. awl loads
   it at startup and swaps in a rebuilt one on ``plugin_restart``. See
   :doc:`reload` and :doc:`api/plugin-abi`.

What lives where
----------------

The file names say which binary a file belongs to: ``awl_*`` (and awl.c,
awl.h) to ``awl``, where awl.c starts and ends it and each awl_*.c is one of
its parts (see :doc:`api/awl`), ``plugins_*`` and plugins/ (and plugins.c, plugins.h) to
``libawlplugins.so``; tray/ is awl's, the icon theme of the programs it
starts with ``TRAY_ICONS``. Every type the library shares with awl is in
awl.h; the headers that make up awl's side of the interface (awl.h,
awl_host.h, awl_plugin_abi.h, awl_draw.h, awl_log.h, awl_dict_type.h) are
included by the libraries too, awl's own types are in awl_core.h.
config.mk's compile-time switches belong to ``awl``, plugins_config.c to the library, which
changes awl's defaults (awl_config_defaults.h) and its own (plugins_config_defaults.h)
with it.

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Part
     - awl (main thread)
     - libawlplugins.so
   * - Bar
     - buffers, layout, pointer events, hover timers (awl_bar.c, awl_draw.c)
     - the widgets: what they show and what clicks do (plugins_widgets.c)
   * - Status data
     - polls the redraw eventfd (awl_redraw.c)
     - the plugins, on the poller thread (plugins/)
   * - Wallpaper
     - shows it, scaled to cover, and fades (awl_background.c); the timer and
       the ``wallpaper``/``wallpapermode`` actions (awl_wallpaper.c)
     - lists the directory, picks and decodes (plugins/wallpaper.c, Wuffs)
   * - Popups
     - awl's own popups (text, or a plugin's painter), any number at once: draws and places them (awl_window.c, awl_popup.c)
     - what it says and when it opens (plugins_widgets.c; for now only the
       temperature widget, as a test)
   * - Launcher
     - a popup that has the keyboard while shown (awl_popup.c's grab,
       awl_input.c hands it the keys before the bindings)
     - the index of .desktop files, the fuzzy matching, the window and its
       keys (plugins/launcher.c, plugins/fuzzy.c, plugins/desktop_entry.c)
   * - Desktop panel
     - scene nodes, buffers, placement (awl_desktop.c)
     - scans ``desktop_dir``, lays out and draws the list, handles
       clicks on the bare desktop (plugins_desktop_panel.c)
   * - Config
     - config.mk's switches, and `awl_config_t` with its defaults (awl_config_defaults.h)
     - `awl_plugin_config_t` with its defaults (plugins_config_defaults.h), and
       the configuration's changes to both (plugins_config.c, its ``CUSTOM``
       part)
   * - Notifications
     - alerts (awl_alert.c)
     - spawns ``notify-send`` (plugins.c)
   * - Screen lock
     - starts the locker, tells when it covers the screen, raises the
       alert when logind isn't listened to (awl_lockscreen.c)
     - listens to logind, holds the inhibitor (plugins/logind.c)

The library never calls into awl directly: awl hands it a table of
functions, `awl_host_t` (awl_host.h), and gets one back, `awl_plugin_api_t`.
It does read some of awl's structs (`awl_draw_t`, `widget_t`, `awl_config_t`,
`Arg`), so both have to be built from the same headers; the library checks
that when it is loaded. `Monitor` and `Client` are opaque to it: what a
bar's widgets need of its monitor awl copies into `awl_draw_t`, so a change
to them is no change of the ABI. The library has no symbol reference into awl; awl exports
nothing.

Logging works the same way. awl_log.c, in awl, writes every message to
stderr and the log file (see `awl_log.h`); in code built with
``AWL_HOSTED`` (the library) `awl_err_printf()` and the other level
macros expand to ``awl_host->log(...)``, which is awl's `awl_dbg_printf()`.
wlroots' messages are routed there too.

A redraw, end to end
--------------------

#. A plugin thread notices a change, e.g. the clock ticks, stores the new
   value in an atomic and calls ``awl_host->redraw_request()``.
#. That is awl's `awl_redraw_request()`, which writes the redraw eventfd. Requests coalesce until the main loop
   gets to it, so a burst costs one redraw.
#. The main loop wakes up and redraws the bars: for each right-hand widget
   it calls `widget_t.measure` to learn its width, then `widget_t.draw` for
   every widget, into a buffer it reuses.
#. The same wakeup updates the desktop panels (`desktop_update_all()`), which
   redraw only if `awl_plugin_api_t.desktop_version` changed, and takes a
   newly decoded wallpaper, if any (`awl_plugin_api_t.wallpaper_take`).

Locking
-------

awl locks with an ext-session-lock client, ``cfg->lock.locker``
(swaylock by default), started by `lockscreen()`: as the ``lockscreen``
action, at start with ``lock_at_start``, and on request of the plugins'
logind thread, which listens for ``loginctl lock-session`` and for an
upcoming suspend. Before suspend it holds a delay inhibitor until the locker
covers every output, so the machine never sleeps, or wakes, showing the
desktop. See :doc:`threads` for the sequence.

If that thread doesn't listen, awl raises an alert (awl_alert.h): white on
red in the bottom left corner of every output, above everything, the lock
screen included (`awl_alert_draw()`), and a critical notification. awl raises it, not the library, so it comes up even without one.

Where to start reading
----------------------

* awl_plugin_abi.h: the contract between awl and the library, and the list
  of what the library does; awl_host.h: awl's side of it, `awl_host_t`.
* plugins/thread.h and awl_redraw.h: how every background thread starts,
  stops and reports.
* awl_draw.h: `widget_t`, the unit the bar is made of.
* config.mk (compile-time switches), awl.h's `awl_config_t` (defaults in awl_config_defaults.h)
  and plugins.h's `awl_plugin_config_t` (defaults in plugins_config_defaults.h),
  both changed by plugins_config.c's ``CUSTOM`` part: the configuration, read at startup and
  reloadable.
