# Plan: launcher and notification daemon in awl

Status: planned (2026-10-20), nothing implemented yet. Replaces fuzzel (`menucmd`)
and fnott (autostart, `fnottctl actions`).

Most of what both need exists already: awl windows with painters, styling and
blur, placement by the bar, icon lookup (icons/icon.h), `actions->spawn`, sd-bus
(awl_sni.c, plugins/sni.c) and the scanner-thread pattern (plugins/thread.h,
plugins_desktop_panel.c). Two things are missing and are most of the work:

1. **Keyboard input.** awl windows have no keyboard focus by design
   (awl_window.h), and `keypress()` only knows bindings and clients.
2. **Popups that aren't menus.** Every window is placed by the bar, hides on a
   press elsewhere and closes the open menus when shown; notifications need
   the opposite on all three.

## Phase 0: core prerequisites (awl, one ABI bump to 27)

### 0a. Keyboard grab

- Host call `window_set_key(win, key, data)`. While a window with this
  callback is shown, `keypress()` hands it every press and release *before*
  the key bindings: keysym, UTF-8 (`xkb_state_key_get_utf8`), modifiers.
- Key repeat through the group's existing `key_repeat_source`, so held
  BackSpace and arrows repeat.
- While grabbed the seat's keyboard focus is cleared
  (`wlr_seat_keyboard_notify_clear_focus`); on hide it goes back to
  `focustop(selmon)`.
- Locking the screen and the window's monitor going away drop the grab.
- awl_bar.c's "not while a layer surface holds the keyboard" check also covers
  the grab.
- First user: the tray menus get Escape and Up/Down/Return/Left/Right
  (tray_menu_window.c), a small first test of the grab and an item that was
  left open anyway.

### 0b. Placement

- Host call `window_show_at(win, m, anchor, dx, dy)`, anchor one of: center,
  top center, a corner of the window area.
- The launcher uses top center. Notifications use a corner; the caller stacks
  them using `window_part_box` / `awl_window_box` heights.

### 0c. Persistent windows

- A per-window flag: not hidden by a press elsewhere, doesn't close open menus
  when shown.
- In the overlay layer, so notifications show above fullscreen clients (a
  config switch).

## Phase 1: launcher (library, `plugins/launcher.{c,h}`)

### Index

- Built on a thread, like the desktop panel's scanner:
  `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`/applications, desktop-file-id
  precedence.
- Respects Hidden, NoDisplay, OnlyShowIn/NotShowIn, TryExec.
- Reads localized Name, GenericName, Keywords, Icon.
- Exec parsed per the spec: quoting, `%f %u %F %U` dropped, `%i %c %k`
  expanded. `Terminal=true` runs through a configured terminal, `Path=` sets
  the working directory.
- Rescans on inotify, so opening is instant.
- Later: a `$PATH` mode (like wmenu-run), desktop Actions.

### Fuzzy matching

- The fzy algorithm, vendored (MIT, ~250 lines): a score plus the matched
  positions, for highlighting.
- Ranking: Name first, then Keywords/GenericName/Exec with a penalty, then
  frecency.
- Frecency from `$XDG_STATE_HOME/awl/launcher` (use count, last use); the
  empty query shows the frecency order.

### Interface

- One painter: an input row (prompt, text, cursor) and N result rows (24 px
  icon, name, dimmed GenericName, matched characters highlighted). The
  painter keeps its own selection; the hover parameter stays as it is.
- Keys:
  - typing: text, BackSpace, Ctrl-W/Ctrl-U;
  - moving: Up/Down, Tab, Ctrl-N/Ctrl-P, Page Up/Down;
  - choosing: Return launches, Escape closes.
- Mouse: hover, click to launch, scroll.

### Icons

- `awl_icons_lookup` at the monitor's scale, SVG allowed, only for the visible
  rows, cached by name and size. Main thread under the existing caps, as for
  the tray.

### Launching and config

- `actions->spawn` with the expanded argv, no shell. Later an xdg-activation
  token (awl supports xdg-activation).
- `awl_plugin_config_t.launcher`: rows, width, icon size, terminal, prompt;
  colors 0 = the popup's. Blur reuses `blur.launcher`.
- Library action `launcher`, replacing `menucmd` (fuzzel) in the bindings.

## Phase 2: notification daemon

### Open decision: where the D-Bus server lives

- **Recommended: core.** A new awl_notifications.c (named so it doesn't clash
  with `awl_notify_config`) owns `org.freedesktop.Notifications` on the core
  sd-bus connection the SNI watcher uses, and holds the store: id counter,
  replaces_id, expiry timers, close reasons. The library only renders: a
  snapshot and a "changed" hook through the host table, calls back for
  dismiss and invoke. Mirrors the watcher/host split; notifications, ids and
  waiting `ActionInvoked` callers survive a plugin reload.
- Alternative: all in the library on the poller loop. Less ABI, but every
  reload drops the bus name and every open notification.

### Spec coverage

- Methods: `Notify`, `CloseNotification`, `GetCapabilities` (body, actions,
  icon-static, later body-markup), `GetServerInformation`.
- Signals: `NotificationClosed` (reasons 1–4), `ActionInvoked`,
  `ActivationToken`.
- The name requested with `SD_BUS_NAME_QUEUE`: a running fnott keeps it if it
  was first, which is logged.

### Hints

- urgency.
- Images:
  - data: `image-data`, `image_data`, `icon_data` (iiibiiay, RGB/RGBA with
    rowstride → pixman, next to `awl_icon_from_pixmaps`);
  - paths: `image-path`, `app_icon` (a path, a `file://` URL or an icon name);
  - fallback: `desktop-entry`, its icon through the launcher's index (shared
    code).
- `transient`, `resident`, `category`.
- Replace in place: `x-canonical-private-synchronous`, `x-dunst-stack-tag`.
- `value`: a progress bar (volume/brightness OSDs).

### Behavior

- Default timeouts per urgency from config; critical never expires; the timer
  pauses while the pointer is over the notification.
- Markup: first strip tags and decode entities; then `<b>`, `<i>`, `<u>`
  (needs a second fcft font).
- Word wrap and ellipsis on top of `text_width`, in the library.

### Interface

- One persistent window per notification, stacked from a corner (default top
  right, under the bar), at most N shown plus a "+k more" row.
- Icon, summary, wrapped body with a line limit, actions as clickable parts.
- Left click: the default action, else dismiss; right click: dismiss; middle
  click: dismiss all.
- Hidden while the screen is locked, shown again after.

### Actions and widget

- Actions: dismiss, dismiss all, the newest one's default action, show its
  actions (replacing `fnottctl actions` on MOD+Shift+N).
- Do-not-disturb: a cfgdict key, plus an optional bar widget with a count.
- Later: a history popup of the last N closed.

## Phase 3: tests

- Launcher: `live.sh key` (wtype) against a fake `$XDG_DATA_DIRS` with a few
  .desktop files; ranking, Exec expansion, the argv spawned.
- Notifications: test/windows.sh-style checks driven by `notify-send` and
  `gdbus`: replaces_id, actions with `ActionInvoked`, image-data, expiry,
  close reasons, behavior across a plugin reload.
- `make check`: fzy scoring and Exec parsing as unit tests.

## Order

1. 0a plus the tray menus' keyboard navigation (small, de-risks the grab).
2. 0b and 0c.
3. The launcher.
4. The notification daemon, once the open decision is made (the biggest
   piece, for the spec's breadth).

## The user's config afterwards

plugins_config.c changes in three places, reported rather than made silently:
`menucmd` becomes the `launcher` action, `fnott` leaves autostart, `fnottctl`
is replaced by the new actions.
