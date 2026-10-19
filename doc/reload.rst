Live reload
===========

``make`` builds ``awl`` and ``libawlplugins.so``. With awl running,
``plugin_restart`` (MOD+Ctrl+r in config_plugins.def.h) swaps in the freshly
built library: new widgets, new plugin code and config_plugins.h,
without restarting awl or any client.

What a reload picks up
----------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Reloads with the library
     - Needs a restart of awl
   * - plugins_widgets.c, plugins_desktop_panel.c, plugins.c, plugins/
     - awl.c, awl_draw.c, awl_redraw.c, awl_lock.c, awl_alert.c, awl_window.c, awl_desktop.c, awl_background.c, awl_plugin_host.c, awl_sni.c (the tray's watcher), tray/
   * - config_plugins.h: keys, buttons, rules, layouts,
       colors, font, borders, blur, input devices, keymap, tray and
       wallpaper settings
     - config_awl.h: tags, the bar's position, autostarts, logging
   * -
     - any change to `awl_host_t`, `awl_plugin_api_t` or to a struct both
       sides read (`Monitor`, `Client`, `awl_draw_t`, `widget_t`, `Arg`), i.e.
       anything that bumps `AWL_PLUGIN_ABI`

Monitor rules apply to outputs that appear after the reload. A library that
was built against a different awl refuses to load (see
`awl_plugin_entry_t`); the old one then stays and its plugins are just
restarted.

The sequence
------------

``plugin_restart`` usually runs from a key binding, i.e. from code in the
library that is about to be unloaded, so it only schedules the reload on the
event loop's next idle round. Then `awl_plugins_reload()`:

#. Copies the library file into a memfd and loads that with ``dlopen()``,
   so every load gets the current file, and a ``make`` rewriting the file
   can't change the copy in use. The Makefile links under a temporary name and
   renames, so a reload during a build sees the old or the new library,
   never half of one.
#. Looks up `AWL_PLUGIN_ENTRY` and calls it with awl's `awl_host_t`; the
   library compares `AWL_PLUGIN_ABI` and the struct sizes and returns its
   `awl_plugin_api_t`, or NULL.
#. Calls awl's ``pluginsdetach()``: the hover popup gets its leave callback,
   every bar's widgets are freed (`awl_draw_widgets_clear()`), and awl falls back
   to its built-in config, since the current one points into the old
   library.
#. Stops the old library's threads (`awl_plugin_api_t.fini`) and unloads it,
   unless a thread is stuck in the kernel (see :doc:`threads`): then the old
   library stays mapped for good.
#. Starts the new library's threads (`awl_plugin_api_t.init`).
#. Calls awl's ``pluginsattach()``: switches to the new library's config and
   applies what changed (``configapply()``), gives every bar its widgets
   again and redraws.

The tray widget's host is one of the library's, so it starts afresh with the
new library and asks awl's watcher, which keeps running, for the items
again. Afterwards awl hands the new tray settings' icon theme to the
programs it starts with ``TRAY_ICONS`` (`awl_tray_app_set_theme()`).

As a sequence, with the library's threads in it:

.. mermaid::

   sequenceDiagram
     participant K as key binding
     participant D as awl main thread
     participant H as awl_plugin_host.c
     participant O as old library
     participant N as new library
     K->>D: plugin_restart()
     D->>D: idle source: pluginrestart()
     D->>H: awl_plugins_reload(detach, attach)
     H->>H: libopen(): memfd copy, dlopen()
     H->>N: awl_plugin_entry(&host)
     N-->>H: its api, or NULL (ABI or sizes differ)
     H->>D: pluginsdetach()
     D->>O: hover popup: callback_leave
     D->>O: awl_draw_widgets_clear(): every widget's free()
     D->>D: cfg = builtinconfig(), layouts moved
     alt the new library loaded
       H->>O: api->fini(): stops its threads
       alt every thread stopped
         H->>O: dlclose(), close the memfd
       else a thread is stuck
         Note over O: stays loaded for good
       end
       H->>H: api = the new one
     else it didn't
       H->>O: api->fini(), the same library goes on
     end
     H->>N: api->init(paused): starts its threads
     H->>D: pluginsattach()
     D->>N: configuse(): api->config(), configapply()
     D->>N: bar_widgets() for every bar
     D->>D: drawbars(), desktop_reloaded()
     D->>D: awl_tray_app_set_theme(cfg->tray->icon_theme)

The library's lifetime
----------------------

awl starts the library's threads at startup and on a reload, and stops them
on a reload and at exit:

.. mermaid::

   flowchart LR
     setup["awl starts:<br/>setup()"] --> load["awl_plugins_load(0)"] --> init1["api->init()"]
     restart["plugin_restart"] --> reload["awl_plugins_reload()"] --> fini2["api->fini()<br/>of the old library"] --> init2["api->init()<br/>of the new one"]
     cleanup["awl exits:<br/>cleanup()"] --> unload["awl_plugins_unload()"] --> fini3["api->fini()"]

What ``api->init()`` and ``api->fini()`` do (plugins.c). The scanner and the
wallpaper thread may get stuck on a dead mount, so they are given 500 ms;
the poller is joined without a limit.

.. mermaid::

   flowchart LR
     subgraph init["api->init(paused)"]
       direction TB
       i1["awl_plugin_start():<br/>ip, stats, temp, bat, date"] --> i2["poller_new()"]
       i2 --> i3["pulse_init(), backlight_init()<br/>on the poller's loop"] --> i4["poller_start():<br/>poller thread"]
       i4 --> i5["awl_desktop_start():<br/>desktop thread"] --> i6["awl_wallpaper_start():<br/>wallpaper thread"]
     end
     subgraph fini["api->fini()"]
       direction TB
       f1["awl_desktop_stop():<br/>up to 500 ms"] --> f2["awl_wallpaper_stop():<br/>up to 500 ms"]
       f2 --> f3["poller_stop(): joined"] --> f4["pulse_free(), backlight_free(),<br/>poller_free(), the rest freed"]
       f4 --> stuck{"a thread<br/>left running?"}
       stuck -- no --> r0["returns 0:<br/>the library may be closed"]
       stuck -- yes --> r1["returns nonzero:<br/>the library stays mapped"]
     end
     init ~~~ fini

The config split
----------------

awl.c includes config_awl.h, and config_plugins.def.h as the fallback for
while no library is loaded. plugins_config.c includes config_plugins.h and turns
it into an `awl_config_t` with `AWL_CONFIG_TABLE`; awl.c does the same with
config_plugins.def.h. Only the library ever sees config_plugins.h.

The functions config_plugins.h binds keys to are awl's; in the library, they
are same-named wrappers that call awl through `awl_host_t.actions`, so the
file reads like a dwl config.h. A function it binds has to be listed in
`AWL_ACTIONS` (or `AWL_ARRANGES` for layouts), or be one of the library's own
actions, which plugins_config.c defines next to the wrappers (``notifyconfig``,
``backlighttoggle``). The wrappers and those actions aren't static, so any
of them may stay unbound without a warning, and the library exports none of
them. config_plugins.def.h, which awl itself is built with, can bind only
`AWL_ACTIONS`.

Adding to the ABI
-----------------

Adding a function to `awl_host_t` or `awl_plugin_api_t`, or changing a
struct both sides read:

#. Change the table in awl_host.h or awl_plugin_abi.h and bump
   `AWL_PLUGIN_ABI`.
#. Fill in the new entry: in awl_plugin_host.c for the host table (awl.c for
   `AWL_ACTIONS`), in plugins.c for the library's.
#. Rebuild and restart awl: the running one would refuse the new library.
