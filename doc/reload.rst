Live reload
===========

``ninja -C build`` builds ``awl`` and ``libawlplugins.so`` (see meson.build). With awl running,
:c:func:`plugin_restart` (MOD+Ctrl+r by default) swaps in the freshly
built library: new widgets, new plugin code and plugins_config.c,
without restarting awl or any client.

What a reload picks up
----------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Reloads with the library
     - Needs a restart of awl
   * - plugins_widgets.c, plugins_desktop_panel.c, plugins.c, plugins/
     - awl.c and its parts, awl_*.c (awl_sni.c is the tray's watcher,
       awl_notifications.c the notification server), tray/
   * - plugins_config.c: keys, buttons, rules, layouts,
       colors, font, borders, blur, input devices, keymap, tray and
       wallpaper settings
     - meson.options: compile-time switches
   * -
     - any change to :c:type:`awl_host_t`, :c:type:`awl_plugin_api_t` or to a struct both
       sides read (:c:type:`awl_config_t`, :c:type:`awl_bar_t`, :c:type:`widget_t`, :c:type:`Arg`; in
       awl.h and awl_bar_type.h), i.e.
       anything that bumps :c:macro:`AWL_PLUGIN_ABI`

Monitor rules apply to outputs that appear after the reload. A library that
was built against a different awl refuses to load (see
:c:type:`awl_plugin_entry_t`); the old one then stays and its plugins are just
restarted.

The sequence
------------

:c:func:`plugin_restart` usually runs from a key binding, i.e. from code in the
library that is about to be unloaded, so it only schedules the reload on the
event loop's next idle round. Then :c:func:`awl_plugins_reload`:

#. Copies the library file into a memfd and loads that with ``dlopen()``,
   so every load gets the current file, and a build rewriting the file
   can't change the copy in use. meson.build links under a temporary name and
   renames, so a reload during a build sees the old or the new library,
   never half of one.
#. Looks up :c:macro:`AWL_PLUGIN_ENTRY` and calls it with awl's :c:type:`awl_host_t`; the
   library compares :c:macro:`AWL_PLUGIN_ABI` and the struct sizes and returns its
   :c:type:`awl_plugin_api_t`, or NULL.
#. Calls awl's :c:func:`pluginsdetach`: the hover popup gets its leave callback,
   every bar's widgets are freed (``awl_draw_widgets_clear()``), and awl falls back
   to its built-in config, since the current one points into the old
   library.
#. Stops the old library (:c:member:`awl_plugin_api_t.fini`): its threads, and it
   gives back its popups and timers. :c:func:`pluginsreap` removes what it left
   of them (with a warning), since their callbacks point into it, and so do
   the ``AWL_TEST_WINDOWS`` popups go. Then it unloads the library, unless a
   thread is stuck in the kernel (see :doc:`threads`): then the old library
   stays mapped for good.
#. Starts the new library's threads (:c:member:`awl_plugin_api_t.init`).
#. Calls awl's :c:func:`pluginsattach`: switches to the new library's config and
   applies what changed (:c:func:`configapply`), gives every bar its widgets
   again and redraws.

The tray widget's host is one of the library's, so it starts afresh with the
new library and asks awl's watcher, which keeps running, for the items
again. Afterwards awl hands the new tray settings' icon theme to the
programs it starts with :c:macro:`TRAY_ICONS` (:c:func:`awl_tray_app_set_theme`).

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
       H->>O: api->fini(): stops its threads, gives back its popups and timers
       H->>D: pluginsreap(): what it left of them
       alt every thread stopped
         H->>O: dlclose(), close the memfd
       else a thread is stuck
         Note over O: stays loaded for good
       end
       H->>H: api = the new one
     else it didn't
       H->>O: api->fini(), the same library goes on
       H->>D: pluginsreap()
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

What ``api->init()`` and ``api->fini()`` do (plugins.c). The scanner, the
launcher's index and the wallpaper thread may get stuck on a dead mount, so
they are given 500 ms;
the poller, which may hang in a sysfs read, and the tray's loop, which
reads icon files, 2 s each. What a stuck one runs or polls is leaked, not
freed.

.. mermaid::

   flowchart LR
     subgraph init["api->init(paused)"]
       direction TB
       i1["awl_plugin_start():<br/>ip, stats, temp, bat, date"] --> i1b["awl_loop_new(), sni_new(),<br/>awl_loop_start(): tray thread"] --> i2["poller_new()"]
       i2 --> i3["pulse_init(), backlight_init()<br/>on the poller's loop"] --> i4["poller_start():<br/>poller thread"]
       i4 --> i5["awl_desktop_start():<br/>desktop thread"] --> i5b["awl_launcher_start():<br/>launcher thread"] --> i6["awl_wallpaper_start():<br/>wallpaper thread"]
     end
     subgraph fini["api->fini()"]
       direction TB
       f1["awl_desktop_stop():<br/>up to 500 ms"] --> f1b["awl_launcher_stop():<br/>up to 500 ms"] --> f2["awl_wallpaper_stop():<br/>up to 500 ms"]
       f2 --> ft["awl_loop_stop():<br/>the tray's, up to 2 s;<br/>its popups go, sni_free() unless stuck"]
       ft --> f3["poller_stop():<br/>up to 2 s"]
       f3 -- exited --> f4["pulse_free(), backlight_free(),<br/>poller_free(), the rest freed"]
       f3 -- stuck --> f5["the rest is leaked"]
       f4 --> stuck{"a thread<br/>left running?"}
       f5 --> stuck
       stuck -- no --> r0["returns 0:<br/>the library may be closed"]
       stuck -- yes --> r1["returns nonzero:<br/>the library stays mapped"]
     end
     init ~~~ fini

The config split
----------------

meson.options' compile-time switches (``scenefx_shatter``) only a rebuild
and restart of awl apply. Everything else is two structs: :c:type:`awl_config_t`
(awl.h) holds what awl itself reads (the library may read it too),
:c:type:`awl_plugin_config_t` (plugins.h) what only the library reads: the bar's
widgets, the launcher's and the notifications' look, the temperature
scales, the battery. The colors are split alike: awl's (borders, popups,
menus, the alert) in :c:member:`awl_config_t.colors` (:c:type:`awl_colors_t`), the library's
(the bar, the launcher, the notifications, ...) in
:c:member:`awl_plugin_config_t.colors` (:c:type:`awl_plugin_colors_t`).

awl_config_defaults.h holds awl's defaults; awl runs with them while no library is
loaded, and hands them to the library as :c:member:`awl_host_t.config_defaults`.
plugin_config_defaults.h holds the library's. The ``CUSTOM`` part of
plugins_config.c is the configuration: its ``configure(c, p)`` changes what it wants in both: a
value (``c->borderpx = 2``), a part (``c->popup.font = "sans:size="``,
``p->colors.win_focused = ...``), or a list, pointed at its own array with
``SETLIST(c->keys, keys)`` or ``SETLIST(p->widgets, widgets)``. On every
(re)load awl fills its copy of :c:type:`awl_config_t` through
:c:member:`awl_plugin_api_t.config`; :c:type:`awl_plugin_config_t` never leaves the library.
Only the library ever sees it.

In bindings, :c:macro:`MODKEY` stands for ``c->modkey``, whatever it is when the
binding is pressed, so changing the modifier needs no new bindings. Monitor
rules and :c:func:`setlayout` take a layout's index, so awl's default bindings and
rules work with the library's layouts too.

The functions the configuration binds keys to are awl's. A binding names
one as ``AWL_ACTION(spawn)``, a layout its arrange function as
``AWL_LAYOUT(tile)`` (:c:macro:`AWL_ACTION() <AWL_ACTION>`, awl.h): in the library that is a
wrapper, ``awl_action_spawn``, that calls awl through :c:member:`awl_host_t.actions`,
in awl the function itself, so the arrays of awl_config_defaults.h can be
copied into the ``CUSTOM`` part as they are. A function a binding names so
has to be listed in :c:macro:`AWL_ACTIONS` (or :c:macro:`AWL_ARRANGES` for layouts), or be one
of the library's own actions, defined next to the wrappers
(:c:func:`notifyconfig`, :c:func:`launcher`, :c:func:`backlighttoggle`); a function defined in the
``CUSTOM`` part is named as it is. The wrappers and those actions aren't
static, so any of them may stay unbound without a warning, and the library
exports none of them. awl's defaults can bind only :c:macro:`AWL_ACTIONS`.

Adding to the ABI
-----------------

Adding a function to :c:type:`awl_host_t` or :c:type:`awl_plugin_api_t`, or changing a
struct both sides read:

#. Change the table in awl_host.h or awl_plugin_abi.h and bump
   :c:macro:`AWL_PLUGIN_ABI`.
#. Fill in the new entry: in awl_plugin_host.c for the host table (the module the
   action belongs to for :c:macro:`AWL_ACTIONS`, awl_config.h declares them), in plugins.c for the library's.
#. Rebuild and restart awl: the running one would refuse the new library.
