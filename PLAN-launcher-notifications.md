# Plan: notification daemon in awl

Status (2026-10-22): the launcher, the keyboard grab, placement, bold and
italic fonts, the markup parser and layout, JPEG (Wuffs v0.4),
`awl_icon_from_rgba()`, the core's D-Bus server, persistent windows (0c)
the library's notification windows (without image files and icon
names), awl's own notifications past D-Bus and the stack fitted to the
window area are done. Open: images from files and names, the actions and the
widget, the launcher's leftovers. Replaces fnott (autostart,
`fnottctl actions`); fuzzel (`menucmd`) is replaced already.

## Done, for reference

- Server: awl_notifications.{c,h} in the core, on its own session-bus
  connection. awl_bus.{c,h} is the connect/process/reconnect loop the SNI
  watcher had, now shared by both: each service connects on its own, so
  one's failure doesn't drop the other's.
  - The record `awl_notif_t` and the host calls `notif_list`,
    `notif_close`, `notif_invoke`, `notif_hold` are in awl_host.h. The
    `notifications_changed` hook in `awl_plugin_api_t` fires once per loop
    round. ABI 32.
  - The library only logs the hook so far (plugins.c).
  - Timeouts per urgency are in `awl_config_t.notifications`.
  - Differs from the earlier plan:
    - A replacement keeps its place, timer and hold.
    - A closed id that was given out comes back as itself.
    - Image data is stored with its rows packed, 64 MiB across all open
      notifications.
    - 256 can be open; the oldest non-critical one is closed with reason 4.
    - Strings are capped: body 16 KiB, summary 1 KiB, paths 4 KiB, the
      rest 256 bytes.
    - On release, the hold restarts the timeout from the beginning.
- Tests: test/notifications.sh, the server driven by `gdbus` in a live.sh
  instance. The library's calls are made through the AWL_TEST_WINDOWS FIFO
  (`nclose`, `ninvoke`, `nhold`; the dump lists `notif` lines).
  - `live.sh run` now exports the test session's `DBUS_SESSION_BUS_ADDRESS`.
- Images: `awl_icon_decode()` takes PNG, JPEG and SVG by their magic
  bytes. Wuffs v0.4.0-alpha.12 is pinned in vendor/wuffs/wuffs.h; move to
  v0.4.0 once it is out.
  - The JPEG decoder's work buffer is capped (`AWL_ICON_MAX_WORK`).
  - `awl_icon_from_rgba()` takes image-data: 8 bits, 3 or 4 channels,
    bounds checked without overflow.
  - All of it is unit tested in test/icon_unit.c, including truncated and
    mutated files, under ASan/UBSan.
- Markup and layout: plugins/markup.{c,h}, test/markup_unit.c.
  - Bold and italic come from `awl_font_style()` (awl_draw.c, host call
    `font_style`).
  - Painting, underline included, is the notification painter's job.
- Persistent windows (0c): host call `window_set_persistent` (ABI 33),
  awl_popup.c.
  - A press elsewhere, `popupshideall()` and other popups shown leave
    them; showing one closes no menu.
  - Their own scene tree above the other popups; right above LyrOverlay
    (over fullscreen clients) with `awl_config_t.popup.persistent_overlay`
    (default 1).
  - Out of sight while the screen is locked (the tree is disabled), back
    after; they don't take keys while locked. The library needn't hide
    them itself.
  - Any popup now takes the pointer only where nothing above it does
    (`popupcovered()`): an ordinary popup under a fullscreen client lets
    the click through to it.
  - Tested in test/windows.sh (`persist`, `lock` FIFO commands; the
    dump's `persistent` line). Checked by hand over a fullscreen kitty.
- The library's windows: plugins/notify.{c,h}, `awl_plugin_config_t.notify`.
  - Diffing by id and generation on `notifications_changed` and in
    `init`; everything copied out of the record (summary, parsed body,
    actions); image-data converted at the image's size and scale in
    `measure()`, from a fresh `notif_list`.
  - The summary bold, the body's markup (styles, colors, underline)
    wrapped to `max_lines`, the value's bar (track in the popups' dim),
    the actions as buttons in rows (not "default": the click).
  - Stacked from `at` on the selected monitor, newest nearest the corner,
    `max_shown` and a "+k more" text window. A replacement redraws in
    place; every change restacks (cheap when nothing moved).
  - Colors per urgency through the new host call `window_set_colors`
    (frame, background, text over the popups'); `window_box` for the
    stacking. Critical: a red frame by default.
  - Left click: the button's action, else "default", else dismiss. Right:
    dismiss. Middle: dismiss all. Hover holds the timeout; a window freed
    while held releases it.
  - Tested in test/notifications.sh ("-- windows"): stacking, buttons,
    clicks, hold, a redraw in place, `max_shown`, a reload. The dump has
    `pcell` lines (a plugin window's parts) and the persistent flag.
- awl's own notifications: `awl_notifications_send()`, the host call
  `notif_send` (ABI 34), `awl_notify_send()` in plugins.c, which
  `awl_notify()` and the alerts (awl_alert.c, from the core) use; no
  notify-send any more. `awl_notif_send_t` carries what D-Bus has no say
  in: `awl_notif_look_t` (max_lines, width, image_size, colors) and a
  timeout for critical ones; a tag replaces in place. Flagged
  `AWL_NOTIF_AWL`: no client replaces or closes it, no signal tells of it.
  Every send uses `AWL_NOTIF_SEND_INIT` so far: their looks are to be
  configured later. Tested with the `nsend` FIFO command.
- The stack fits the window area (`window_room`, the margins off): the
  last window that fits is cut to the room left (the body's lines, the
  last ending in "…"); "+k more" always fits under it. Each restack starts
  whole again. Tested in "-- the room".
- Launcher (phase 1), keyboard grab (0a), placement (0b).
  - Still open for the launcher: desktop Actions, the xdg-activation
    token, a live.sh-driven launcher test.

## The library's notification windows: what is left

### Images from files and names

- The order of `awl_notif_t`: image_data (done), image_path, app_icon,
  then desktop_entry's Icon through the launcher's index.
- A path or a `file://` URL (percent-decoded) is decoded with
  `awl_icon_decode()`. Anything else is an icon name for
  `awl_icons_lookup()` with the desktop's themes.
- Files and names are decoded on a worker (plugins/thread.h, as the
  launcher's icons do). A notification shows only once its image is done or
  has failed, bounded by ~100 ms, so it doesn't change size right after it
  appears.
- Still open: `AWL_ICON_MAX_DIM` (1024) and `AWL_ICON_MAX_BYTES` (2 MiB)
  refuse screenshots and photos, and Wuffs can't decode scaled down. That
  needs a separate, larger cap for notification image files (and the work
  buffer with it), on the worker.
- Decoded at the monitor's scale, so moving to another output stays sharp;
  symbolic icons in the text color, as in the tray.

### Loose ends

- A scale or font change on the monitor redraws the windows in awl, but
  doesn't restack them: they may overlap until the next change. A host
  hook (or `window_set_hidden`-like "resized" callback) would fix it.
- The stack stays on the monitor it was placed on until the next change,
  even when the selection moves.
- Fitting: a window that would fit whole as the last is cut anyway when
  more come after it (room is kept for "+k more"). Critical ones can end
  up behind "+k more" like any other.
- awl's own looks: which of awl's notifications get which
  `awl_notif_look_t` (config, wallpaper, alerts), and from where (the
  plugin config).
- Blur: the windows blur as the popups (`blur.launcher`); maybe their own
  `blur.notifications` (already in `awl_config_t`, for fnott's layer
  surfaces) once fnott is gone.
- Do-not-disturb: a cfgdict key the library reads (it survives reloads).
  Non-critical notifications aren't shown while it's on; the core still
  stores, answers and expires them.

### Actions and widget

- Library actions: dismiss, dismiss all, the newest one's default action,
  and show its actions (replacing `fnottctl actions` on MOD+Shift+N).
- Do-not-disturb toggle, plus an optional bar widget with a count.
- Later: a history popup of the last N closed (not the transient ones;
  the core would have to keep them).
- Later: the xdg-activation token for `notif_invoke`.

## Tests still to write

- test/notifications.sh: images from files and names once they exist;
  markup and the bar only by eye so far (a screenshot check).
- The launcher: `live.sh key` (wtype) against a fake `$XDG_DATA_DIRS`.

## Order

1. Images from files and names, on the worker.
2. Actions, do-not-disturb, the widget; the config changes below.

## The user's config afterwards

plugins_config.c changes, reported rather than made silently: `fnott`
leaves autostart, and `fnottctl` is replaced by the new actions.
