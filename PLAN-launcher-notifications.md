# Plan: notification daemon in awl

Status (2026-10-22): the launcher, the keyboard grab, placement, bold and
italic fonts, the markup parser and layout, JPEG (Wuffs v0.4),
`awl_icon_from_rgba()`, the core's D-Bus server, persistent windows (0c),
the library's notification windows, awl's own notifications past D-Bus,
the stack fitted to the window area and images from files and icon names
(on a thread), the menu, do-not-disturb, the bar's widget, the history
and the idle pause are done.
Open: awl's own looks, blur, the launcher's leftovers. fnott and fuzzel
are replaced already.

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
  - The summary bold, then the app's name in italic braces
    (`notify.show_app`, default on; the summary is cut first, down to
    half the line), the body's markup (styles, colors, underline)
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
- Images from files and names: plugins/notify_image.{c,h}, a thread
  started with the first wish, stopped with the windows (stuck: reported
  to the reload as the others).
  - image_data first (converted on the main thread, as before), else the
    first of image_path, app_icon, desktop_entry's Icon that yields one: a
    path, a `file://` URL (percent-decoded, localhost only) or an icon
    name in the desktop's themes (`tray.icon_theme` first). The desktop
    entry is `<id>.desktop` in the applications directories, a plain file
    name only.
  - Asked for at the monitor's scale (new host call `monitor_scale`, ABI
    35) and the uncut square's size; anything else is drawn scaled until
    the right size comes, so a cut window doesn't ask anew each restack.
    Symbolic icons in the text color.
  - A new window shows once its image came or failed, 100 ms at most
    (polled every 10 ms meanwhile); a replacement keeps showing what it
    had until a changed source's image comes.
  - icons/icon.h: `awl_icon_limits_t` and `awl_icon_decode_limited()`,
    `awl_icon_load_limited()`. Notifications' files may have 16 MiB, 12.6
    megapixels (4096 x 3072; 8192 a side), 64 MiB of decoder memory (a
    12 MP progressive 4:2:0 JPEG needs 52, a 4:4:4 one 105: refused); SVGs
    stay at 2 MiB. Large images are box-shrunk to about twice the size
    before the Lanczos pass (a 3000x2000 PNG to 48 px: 0.1 s, was over 2
    s).
  - The EXIF orientation (a JPEG's APP1, a PNG's eXIf) is applied to every
    decode, after scaling (fitted on its side for 5 to 8); tray and
    launcher icons too. test/icon_unit.c "-- orientation": all eight on a
    four-colored JPEG.
  - A cache on the thread: a file at a size, keyed by its path and what
    stat() says (inode, size, mtime), 32 entries and 8 MiB of pixels, LRU;
    failures are kept too. The main thread gets a copy (pixman's reference
    count isn't atomic).
  - Files are opened with O_NONBLOCK and must be regular: a FIFO as
    image_path no longer hangs the reader (the tray's and launcher's
    reads gain the same).
  - Tested in test/notifications.sh ("-- images", with its own icon theme
    and desktop entry through `$XDG_DATA_DIRS`; the cache, a changed file,
    a 15 MP PNG refused) and test/icon_unit.c ("-- large").
- The menu, do-not-disturb, the widget: plugins/notify_menu.{c,h}, no key
  bindings (a notification's actions are its buttons).
  - Do-not-disturb is the config dictionary's `notify_dnd` (survives a
    reload): only critical ones show, and those asked for (shown again
    from the history, or an open one's "Show" in the menu); the rest stay
    open and in the menu.
  - The menu, a painter of its own in the menus' style: "Do not disturb"
    (on/off), "Dismiss all", then each notification's title (bold, the app
    in italic) and its actions indented. A title: left its default action,
    else dismiss; right dismiss. An action: invoked, the menu closes;
    everything else leaves it open and it follows the changes. Rows that
    don't fit end in "+k more" (not clickable: a text window's "…" row
    would map to a real row).
  - The notification windows stay while it is open: an open menu is
    above the persistent popups (awl_popup.c), clicks included.
  - `AwlWidgetNotifications` (in the default widgets, left of the clock):
    🔔 or 🔕, a red dot at its top right while any are open. Left:
    do-not-disturb; right: the menu under it; middle: dismiss all. A click
    on "+k more" opens the menu in the stack's place.
  - The menu closes 300 ms after the pointer left it and the widget (the
    widget's `callback_view`); opened from "+k more", only once the
    pointer was in it.
  - Tested in test/notifications.sh ("-- the menu"; the dump's new
    `widget` lines). `look()` there now dismisses awl's alerts: the logind
    one comes a while after the start (the test bus has no logind) and
    landed on a check now and then.
- The history: the core keeps the last 100 closed (`awl_notif_past_t`,
  host calls `notif_history`, `notif_forget`, ABI 36): expired, dismissed,
  evicted; not closed by their client, by an action or transient. In
  memory, across reloads. The menu lists them after the open ones, dimmed,
  with the time they closed, under "Clear history" and the count; a right
  click forgets one. Tested in "-- the history" (dump `past` lines, FIFO
  `nforget`).
- Show again: a "Show" button on each past row of the menu (its own
  painter part) opens it again and closes the menu (host call
  `notif_show`, ABI 37). The history keeps the whole notification for it,
  without its actions (no client waits on them) and with at most 16 MiB of
  image data for all entries, the oldest's dropped first. Shown again it
  gets a new id and AWL_NOTIF_SHOWN: no client replaces or closes it, no
  signal tells of it; its timeout starts over; closed again, it goes back
  into the history. Tested in "-- the history" (FIFO `nshow`) and the
  menu section (dump `ppart` lines).
- The idle pause: after `notifications.idle_pause` (10 s) without a key or
  pointer motion the timeouts stop; the next activity resumes them with
  what was left at the last one, at least half the timeout. Defaults now
  10 s (low) and 20 s (normal). Tested in "-- idle" (FIFO `nidle`,
  `nactive`; every other FIFO command but the dump counts as activity).
- Launcher (phase 1), keyboard grab (0a), placement (0b).
  - Still open for the launcher: desktop Actions, the xdg-activation
    token, a live.sh-driven launcher test.

## The library's notification windows: what is left

### Loose ends

- awl's own looks: which of awl's notifications get which
  `awl_notif_look_t` (config, wallpaper, alerts), and from where (the
  plugin config).
- Blur: the windows blur as the popups (`blur.launcher`); maybe their own
  `blur.notifications` (already in `awl_config_t`, for fnott's layer
  surfaces), now that fnott is gone.

Left as they are (decided): a wish for a closed notification is still
decoded (on the thread, so it costs nothing that matters); a desktop entry
is read anew each time; a scale or font change doesn't restack; the stack
stays on its monitor until the next change; the last window is cut to
keep room for "+k more", critical ones included.

### Later

- The history on disk (across awl restarts)? Left out so far: it would
  write message contents to disk.
- The xdg-activation token for `notif_invoke`. A Wayland client may
  not focus itself; to bring up its window when an action is clicked (the
  chat a message came from), the server hands it a token first (the
  spec's `ActivationToken` signal, before `ActionInvoked`), and the client
  passes the token to `xdg_activation_v1.activate` for its window. awl
  would create the token for the click (wlr_xdg_activation_v1 does that)
  and, in `urgent()` (awl_client.c), focus a window that comes with one of
  its own tokens, instead of only marking it urgent as now. Without it
  the window turns urgent and the user has to go there.

## Tests still to write

- test/notifications.sh: markup, the bar, the title and symbolic icons
  only by eye so far (a screenshot check).
- The launcher: `live.sh key` (wtype) against a fake `$XDG_DATA_DIRS`.

## Order

1. awl's own looks; blur.
