# Plan: notification daemon in awl

Status (2026-10-21): the launcher, the keyboard grab, placement, bold and
italic fonts, the markup parser and layout, JPEG (Wuffs v0.4),
`awl_icon_from_rgba()` and the core's D-Bus server are done. Open: 0c
(persistent windows), the library's notification windows, the actions
and the widget, the launcher's leftovers. Replaces fnott (autostart,
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
- Launcher (phase 1), keyboard grab (0a), placement (0b).
  - Still open for the launcher: desktop Actions, the xdg-activation
    token, a live.sh-driven launcher test.

## 0c. Persistent windows (core)

- A per-window flag: the window isn't hidden by a press elsewhere, and
  showing it doesn't close open menus.
- In the overlay layer, so notifications show above fullscreen clients (a
  config switch).

## The library's notification windows

### Diffing

- On `notifications_changed` and in `init`, read `notif_list()`. Diff by id
  and generation:
  - a new id gets a window;
  - a new generation means a redraw;
  - a missing id hides and frees its window.
- Pointers from the list stay valid only until the library returns to awl's
  loop or calls `notif_close`/`notif_invoke`. Copy what outlives that (the
  parsed markup, the decoded image).
- Do-not-disturb: a cfgdict key the library reads (it survives reloads).
  Non-critical notifications aren't shown while it's on; the core still
  stores, answers and expires them.

### Images

- The order of `awl_notif_t`: image_data (`awl_icon_from_rgba()`, on the
  main thread), image_path, app_icon, then desktop_entry's Icon through the
  launcher's index.
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
- Decoded at the monitor's scale, so moving to another output stays sharp.
  Drawn left of the text at `image_size` (default 48 logical px), aspect
  kept, centered in that square; symbolic icons in the text color, as in the
  tray. Without an image the text starts at the padding.

### Text

- The summary is plain and drawn bold. The body goes through
  `awl_markup_parse()` and `awl_markup_layout()` with the painter's font in
  four styles (`font_style`), `max_lines` from the config.
- Each run is drawn with `text_over` at `baseline - font->ascent`, in its
  color or the urgency's fg. Underline is a pixman fill at the font's
  `underline` position and thickness.
- The layout is cached per notification and font, and redone when the
  scale or the generation changes.

### Progress bar

- Shown when `value` >= 0: a bar under the body, the full width of the text
  column, `progress_height` high, rounded. The track is in the dim color;
  the filled part is in `progress_color` (0: the urgency's frame color).
- Volume and brightness OSDs replace their notification several times a
  second. A new generation with the same size only redraws: no re-stacking,
  no move.

### Interface

- One persistent window (0c) per notification, stacked from a corner
  (default top right, under the bar) with `window_show_at`. At most N are
  shown, plus a "+k more" row.
- Image, bold summary, wrapped body, the progress bar, and the actions as
  clickable parts (not "default", which is the click on the notification).
- Config, `awl_plugin_config_t.notify`: at, dx/dy, gap, width,
  image_size, max_lines, max_shown, colors per urgency (fg, bg, frame),
  progress_color, progress_height; colors 0 = the popup's. Blur through a
  new `blur.notify`. The timeouts are `awl_config_t.notifications`
  (core).
- Left click: the default action (`notif_invoke`), else dismiss
  (`notif_close`, reason 2). Right click: dismiss. Middle click: dismiss
  all.
- Hover: `notif_hold`.
- Hidden while the screen is locked, shown again after.

### Actions and widget

- Library actions: dismiss, dismiss all, the newest one's default action,
  and show its actions (replacing `fnottctl actions` on MOD+Shift+N).
- Do-not-disturb toggle, plus an optional bar widget with a count.
- Later: a history popup of the last N closed (not the transient ones;
  the core would have to keep them).
- Later: the xdg-activation token for `notif_invoke`.

## Tests still to write

- test/notifications.sh, once the windows exist: windows shown and stacked
  in the dump's `pwin` lines, clicks through `live.sh click`, behavior
  across a reload; markup, images (PNG, JPEG, image-data, an icon name)
  and a `value` bar by screenshot.
- The launcher: `live.sh key` (wtype) against a fake `$XDG_DATA_DIRS`.

## Order

1. 0c.
2. The windows: diffing, text and the progress bar first; images from
   image-data, then files and names on the worker.
3. Actions, do-not-disturb, the widget; the config changes below.

## The user's config afterwards

plugins_config.c changes, reported rather than made silently: `fnott`
leaves autostart, and `fnottctl` is replaced by the new actions.
