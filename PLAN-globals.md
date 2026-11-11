# Fewer globals

Goal: awl's state lives in a few structs that are allocated once and passed
in, so a function's signature says what it touches.

## What there is (from `nm` of build-dev)

The core has about 40 exported globals (`extern` in awl_server.h, awl_output.h,
awl_client.h, awl_cursor.h, awl_input.h, awl_lockscreen.h, awl_popup.h,
awl_config.h, awl_xwayland.h) and about 120 file-static ones.

- **The wlroots objects** (awl_server.h): `dpy`, `event_loop`, `backend`,
  `session`, `renderer`, `alloc`, `compositor`, `scene`, `layers`, `root_bg`,
  `seat`, `idle_notifier`. awl.c also has ~15 static protocol managers and
  ~25 static `wl_listener`s.
- **Shared module state**, used across files:
  - outputs: `mons`, `selmon` (137 uses), `sgeom`, `output_layout`,
    `output_mgr`, `in_cleanupmon`
  - clients: `clients`, `fstack`, `exclusive_focus`, `borders`, `activation`
  - cursor: `cursor` (133 uses), `cursor_mgr`, `cursor_mode`, `grabc`,
    `drag_icon`, the constraints
  - lock: `locked` (13 files)
  - popups: `popups` (16 files)
  - config: `cfg` (252 uses), `cfgdict`
- **Module-private statics**, which only one file sees:
  - awl_lockscreen.c (16 of them)
  - awl_bar.c's hover state
  - awl_config.c's environment
  - awl_wallpaper.c, awl_plugin_host.c, awl_alert.c, awl_background.c,
    awl_desktop.c, awl_sni.c, awl_notifications.c
- **The library** (libawlplugins): `awl_host` and `plugin_data`
  (`awl_plugin_get()`). Most modules already keep one static struct each
  (`N`, `M`, `O`, `W`, `L`, `C`, `T`), which a reload resets.

## Proposal

1. **`awl_server_t`** (awl_server.h), one instance, made by `setup()`:
   - the wlroots objects and the protocol managers;
   - awl.c's listeners, embedded as members. Handlers get the server through
     `wl_container_of(listener, srv, new_output)` (tinywl's pattern), not
     through a global.
   - `cfg` and `cfgdict` as `srv->cfg` / `srv->cfgdict`. A reload swaps the
     pointer, as it does now.
2. **Shared module state as member structs** of the server: `srv->out`
   (mons, selmon, sgeom, layout, mgr), `srv->clients` (lists, exclusive_focus,
   borders), `srv->cursor` (cursor, mode, grab, constraints), `srv->popups`.
   The layouts are public because many files read them.
3. **Module-private state stays private**: each module gets
   `struct awl_<mod> *` in the server, defined only in its .c, allocated in
   its init and freed in its fini. Others go through functions; for example
   `locked` becomes `awl_lock_active(srv)`. This covers lockscreen, bar,
   wallpaper, alert, background, desktop, sni, notifications, plugin_host and
   the config environment.
4. **Passing it in:**
   - Every function that needs server state takes `awl_server_t *srv` first,
     also when it takes a `Client *` or `Monitor *`. That way the signature
     says it touches the server, and it works when the object is NULL
     (`focusclient(srv, NULL, 0)` unfocuses; `focustop(srv, NULL)` walks the
     focus stack with no monitors).
   - Objects that own `wl_listener`s (`Client`, `Monitor`, `LayerSurface`,
     `KeyboardGroup`, `InputDevice`, `SessionLock`, `PointerConstraint`) get a
     `srv` back pointer, because their callbacks only get the object from
     `wl_container_of`. Only the callback reads `x->srv`; it passes `srv` on.
     Server listeners use `wl_container_of(listener, srv, listen.x)`.
   - Why not back pointers instead (checked before B): of the 193 functions
     that used `awl` after A, only 31 take a `Client *` or `Monitor *`, so
     the pointer saves few arguments; several of those take NULL; and
     `drawbar(m)` reaching `m->srv` hides what it touches as much as the
     global did.
   - Actions become `void (*)(awl_server_t *, const Arg *)` and arranges
     `(awl_server_t *, Monitor *)`. plugins_config.c only names
     `awl_actions.*`, so the user config doesn't change.
5. **Process-wide state goes in one global struct**, `awl_process_t awl_proc`
   (awl_process.h). It is the only global in the core, and the place to look
   for anything that signal handlers or other threads reach without a
   parameter:
   - the log (fd, mutex, level, size);
   - awl_spawn.c's pids (the SIGCHLD handler);
   - awl_redraw.c's fd (written from other threads);
   - awl_draw.c's fcft init flag and font ring;
   - the warn-once flags (now function statics, they become a bitset);
   - awl_dict.c's tombstone marker.

   Each field's doc comment says who touches it and from which thread. The
   per-compositor state stays in the passed `awl_server_t`, and nothing in
   `awl_proc` points into it.
6. **The library:**
   - It keeps `awl_host` as its single root: it is the ABI, set once per
     load.
   - The per-file module structs are already the right shape. By the same
     rule they move into `awl_plugin_data_t`, as members or opaque pointers,
     so it is the library's one place for state. The function statics
     (`stamped`, `persistent.c`'s buffers, the widgets' `*_prev`) move there
     too.
   - This side is lower priority.

## Order (each step builds and runs)

Status: A is done (commit 9a9bd18): `awl_server_t` and its member structs
(`awl_outputs_t`, `awl_clients_t`, `awl_cursor_t`) are in awl_server.h, the
instance is awl.c's `server`, reached through `awl`.

B's parameters are done (uncommitted) for the whole core: every function
that needs the server takes `awl_server_t *srv` first; listeners get it from
`wl_container_of` on `srv->listen` (XWayland's two included now) or from
their object's back pointer (`Client`, `Monitor`, `LayerSurface`,
`KeyboardGroup`, `SessionLock`, `PointerConstraint`, and the small
`DragIcon`/`IdleInhibitor` listener structs); timers, idles, fds, signals and
the notification bus are registered with `srv` as their data. `TAGCOUNT(cfg)`
and `TAGMASK(cfg)` take the config. `awl_plugins_reload()` hands `srv` to
detach/attach.

The ABI bump is done too (`AWL_PLUGIN_ABI` 42): `awl_host_t.srv`
is the server, opaque to the library, which passes it back as the first
argument of the host functions that need it (the windows, notifications,
timers, focus and monitors); actions are `(srv, arg)` (a binding gets the
core's `srv`), arranges and `Layout.arrange` `(srv, m)`.
`awl_plugins_load(srv, ...)` sets `host.srv`. The `host_*` wrappers are gone.

The core reads `awl` only in `main()` now.

The private module state (3.) is done (uncommitted, `AWL_PLUGIN_ABI` 43):
- Opaque, a pointer in the server each, the struct defined in the module's
  .c (forward-declared in awl_server.h): `srv->lock`, `bars`, `wallpaper`,
  `alerts`, `input`, `conf` (the library's config and the pending
  plugin_restart), `notifs`, `test`.
- Modules that don't need the server take their own handle, which the
  server keeps: `background_*(srv->background, ...)`,
  `desktop_*(srv->desktop, ...)`, `awl_sni_watcher_stop(srv->sni)`. Their
  callbacks get the handle as data.
- Small state of modules that already had a public member struct went
  there: the cursor's grab offset, active constraint and swallowed release
  (`awl_cursor_t`), the applied border width (`awl_clients_t`); the popups
  became `awl_popups_t` (list, trees, hovered, keyboard). awl.c's
  `redraw_source` is a server member.
- `locked` stays a plain member: ten files read it, awl_test.c sets it.
- The notifications' `notif_list`, `notif_close`, `notif_history` and
  `notif_forget` take `srv` now, hence ABI 43.
- Teardown: where callbacks still reach a module after its fini (monitors
  go with the backend, locks with the clients), a separate free runs last:
  `barfree`, `inputfree`, `awl_lock_fini`, `awl_alert_fini`, `testfini`.
  `background_fini` and `desktop_fini` run right after the backend is gone.

Left for C, because it's process state, not the compositor's: the lock's
eventfd and the logind thread's atomics (awl_lockscreen.c), the plugin host
(one library per process, which keeps one `awl_host`: the handle, its api,
the host table, its timers, paused/covered), the config's environment
(setenv is the process'), `builtinconfig()`'s cache, and the log, spawn,
redraw, draw and dict state listed in 5.

C is done (uncommitted): `main()` allocates the server and frees it after
`cleanup()`; `serverinit()` gives it its empty popup list and its
listeners' handlers (the static initializer before). The `awl` global is
gone. `awl_process_t awl_proc` (awl_process.h, defined in awl_process.c)
holds the process state, one member per module, each documented with its
threads: `log`, `spawn`, `redraw_fd`, `lock` (the eventfd and the logind
atomics), `fonts` (awl_draw.c's ring; its entry type is `awl_font_slot_t`
now), `plugins` (handle, memfd, api, paused/covered, the timers, and the
live `awl_host`, copied from awl_plugin_host.c's const `host_template` on
the first load), `config` (the saved environment, the import flags,
`builtinconfig()`'s cache), `tray` (tray/app_env.c's `E`) and `warned`,
the warn-once bits (`AWL_ONCE(bit)`, for awl_draw.c's two function
statics). Deviations from 5.: the dict's tombstone is only an address, so
it became a `static const char` instead of a member; tray/app_env.c's state
went in too (it is awl's only, the shim is tray/env_shim.c). The command
arrays of awl_config_defaults.h and awl_alert.c's names are `*const` now.
`objdump -t` shows `awl_proc` as the core's only object in .data/.bss; the
rest is const (.data.rel.ro). The unit tests link awl_process.c.

6. is done (uncommitted; the ABI is unchanged, the library's entry points
wrap what they call): `objdump -t` of libawlplugins shows `awl_host`, the
root pointer `plugin_data` and persistent.c's file lock as the only writable
data (besides vendor/'s stb and wuffs tables). plugins/handles.h names the
structs and states the rule:
- Each module's state is a struct defined in its .c, made by its start and
  freed by its stop, a pointer in `awl_plugin_data_t` (osd, calpopup and its
  calendar thread, batpopup, sysmon, notify with notify_menu and
  notify_image, wallpaper's thread (`awl_wallpaper_thread_t`: the core has a
  `struct awl_wallpaper`), the desktop panel, logind, the launcher, the tray's
  menu window and tooltip, the widgets' shared state). Its functions take it,
  its callbacks get it as data, it keeps the root as `->p` where it reaches
  other modules. NULL (out of memory) is a no-op everywhere.
- The entry points (api, actions, bar widgets) start from
  `awl_plugin_get()` / `AWL_PLUGIN_MODULE( m )`; no module calls them.
- The configs moved into the root (`awl_config()` reads it). No thread reads
  them: sni copies its icon theme before its loop starts, as the launcher
  and notify_image already did.
- Threads own their structs (a stuck one's is leaked), never the root.
- sni's `the`, and sni_menu's list and serial: async calls get a cookie
  (tray, serial) freed with their slot (`sni_call_method_async()`); matches
  and timers owned by an item or menu get it directly. The menus' list is
  in the tray.
- Scratch moved: fuzzy.c's 1.3 MB tables are the caller's
  (`awl_fuzzy_tables_t`, the launcher's), persistent.c's buffers are
  malloc()ed per call, function-static arrays became locals; qsort
  comparators that read globals sort by precomputed keys instead.
- cal_eds.c's libecal table is the backend's (dlopen() is refcounted and
  RTLD_NODELETE, so each backend loads its own cheaply); its lock and load
  state are gone.
- `t_plugins_loaded` is stamped by the entry point, once per copy of the
  library, instead of through a function-static flag.

A. **Gather (mechanical):**
   - Add `awl_server_t` and its member structs.
   - Keep one temporary `awl_server_t *awl` global and rewrite `selmon` to
     `awl->out.selmon` and so on, file by file.
   - Move the listeners into the struct.
   - About 40 externs become one.

B. **Pass (needs judgment, module by module):**
   - Add the `srv` parameters (and the back pointers of 4.), starting at the
     leaves (cursor, output, client), then awl.c, then the actions.
   - Make the private modules opaque (3.).

C. **Remove the `awl` global.** Wherever it is still needed, a function
   lacks a way to reach its state. Gather the process-wide state into
   `awl_proc` (5.). Then `nm` shows no data symbols in the core except
   `awl_proc` and const tables.

## To consider

- ABI bump: action signatures and the `awl_actions_t` / `awl_arranges_t`
  tables.
- Thread rule: nothing off the main thread may take `srv`. The poller, image
  and wallpaper threads and the bus already reach awl only through
  `awl_host` and the redraw fd. Document this in doc/threads.rst.
- Doc comments move with their variables onto the struct members, so the
  hawkmoth strict build stays clean (the docs reference e.g. `selmon` and
  `cfg`).
- Teardown order in `cleanup()` becomes explicit: each module's fini frees its
  own state.
- Don't do 4. for awl.c's huge handlers mid-way: step A already makes
  awl.c's dependencies visible as `awl->`, and B can follow per handler.
- dwl heritage: diffs against upstream dwl get harder. That stopped
  mattering a while ago, but it is worth knowing.
