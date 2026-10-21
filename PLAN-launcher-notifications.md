# Plan: launcher and notification daemon in awl

Status (2026-10-21): 0a, 0b, 0d, the launcher (phase 1) and the markup
parser with its layout are done; 0c, the rest of the notification daemon
(phase 2: the core's server, images, the windows) and the rest of phase 3
are open. The D-Bus server lives in the core. Replaces fuzzel (`menucmd`)
and fnott (autostart, `fnottctl actions`).

Done, and how it differs from the plan below:

- 0a: `window_set_key` (presses and repeats only, no releases; what the
  callback doesn't take goes to the bindings), the grab in awl_popup.c
  (`popupkeyboard()`, `popupsgrabupdate()`), `focusclient()` holds back
  while it is held, locking hides the popups. Tray menus: Up/Down/Tab,
  Right/Left, Return/space, Escape.
- 0b: `window_show_at`, `awl_window_place_at()` (unit tested); also
  `monitor_selected` in the host table. ABI 27.
- Launcher: plugins/launcher.c, plugins/desktop_entry.c (parsing, Exec;
  unit tested), plugins/fuzzy.c (unit tested). Not fzy: an fzf-style scorer
  of its own that also allows typos (wrong, extra, swapped characters) on a
  budget by query length, only inside runs of exact matches, with an LCS
  prefilter. Icons are looked up and decoded on the index thread, not the
  main one. Frecency in `$XDG_STATE_HOME/awl/launcher`. Path= goes through
  `sh -c 'cd -- "$0" && exec "$@"'`, since spawn has no working directory.
- Not yet: the `$PATH` mode, desktop Actions, the xdg-activation token, a
  live.sh-driven launcher test (it was tested by hand in live.sh).
- 0d: `awl_font_style()` in awl_draw.c, host calls `font_style` and
  `font_release`, ABI 31 (0c bumps again). test/font_unit.c checks real
  Bold/Oblique faces, shared entries, held fonts surviving eviction, emoji.
  A weight or slant in the font's own name wins over the style.
- Markup and layout: plugins/markup.{c,h}, test/markup_unit.c (with a
  20 000-input random run; clean under ASan/UBSan). Differs from below:
  only known tag names are tags (`a<b2>c`, `x <y and z> w` stay text);
  Pango's `<span foreground|fgcolor|color>` sets a color like `<t>`, and
  `<s> <tt> <sub> <sup> <small> <big>` are dropped with their text kept;
  `&nbsp;` too; tabs become spaces, control characters go. Input is capped
  at 16 KiB, nesting at 16. The ellipsis is measured in the widest font.
  Painting (underline included) is left to the notification painter.
- JPEG: Wuffs v0.4.0-alpha.12 vendored (pinned, see vendor/wuffs/wuffs.h;
  v0.3 is gone, the PNG code only needed `set_quirk`). `awl_icon_decode()`
  takes JPEG by its magic bytes, through the same Wuffs path as PNG, with
  the decoder's work buffer capped (`AWL_ICON_MAX_WORK`, 12 MiB: the worst
  case within `AWL_ICON_MAX_DIM`) before anything is allocated.
  test/icon_unit.c with fixtures in test/data: baseline, progressive,
  4:4:4, gray, CMYK, PNG; arithmetic coding and 1100 px refused; every
  truncation and 10 000 mutated files, clean under ASan/UBSan.
  Still open for notifications: `AWL_ICON_MAX_DIM` (1024) and
  `AWL_ICON_MAX_BYTES` (2 MiB) refuse screenshots and photos; Wuffs can't
  decode scaled down, so a separate, larger cap for notification images
  (and the work buffer with it), on the worker thread.

Most of what both need exists already: awl windows with painters, styling and
blur, placement by the bar, icon lookup (icons/icon.h), `actions->spawn`, sd-bus
(awl_sni.c, plugins/sni.c) and the scanner-thread pattern (plugins/thread.h,
plugins_desktop_panel.c). Two things are missing and are most of the work:

1. **Keyboard input.** awl windows have no keyboard focus by design
   (awl_window.h), and `keypress()` only knows bindings and clients.
2. **Popups that aren't menus.** Every window is placed by the bar, hides on a
   press elsewhere and closes the open menus when shown; notifications need
   the opposite on all three.

## Phase 0: core prerequisites (awl; 0a/0b were ABI 27, 0d is 31)

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

### 0d. Bold and italic in the font registry

For notification markup (`<b>`, `<i>`); the summary is bold too. Done,
ABI 31.

- `awl_font_style(font, style)`, style a mask of `AWL_FONT_BOLD`,
  `AWL_FONT_ITALIC`; 0 hands back font with a new reference. It finds font's
  ring entry and looks up the same spec, scale and dpi with the style added
  to the attrs: `:weight=bold`, `:slant=italic`. Fontconfig picks the
  family's Bold/Italic face, or synthesizes one through its usual rules
  (embolden, an oblique matrix); fcft applies both.
- The style is part of an entry's key, next to `emoji`; `attrs[24]` grows
  to fit (`dpi=…:weight=bold:slant=italic` is 38 characters).
- If the styled font doesn't load, font comes back (with a reference), so
  callers needn't fall back themselves. Emoji entries aren't styled: font
  comes back.
- A host call `font_style` (the library has the painter's font, not its
  name); it gives back with the existing release path, which needs a host
  call `font_release` too (the library has none yet).
- Slots: up to three more per font and scale in use. For the notification
  font on two scales that is six of 32; the ring evicts unused ones.
- Unit test: styled lookups share entries, release counts, failures give
  the plain font back.

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

### Split: the D-Bus server in the core, rendering in the library

Decided: the core. It mirrors the SNI watcher/host split, and the bus name,
the ids, open notifications and the actions clients wait on survive a
plugin reload.

- **Core, awl_notifications.{c,h}** (named so it doesn't clash with
  `awl_notify_config`): owns `org.freedesktop.Notifications` on the core
  sd-bus connection the SNI watcher uses (`SD_BUS_NAME_QUEUE`, see below).
  It parses `Notify` (hints, actions, image-data checked and copied as in
  Images) and holds the store: the id counter, replaces_id and the stack
  tags, expiry timers (`wl_event_loop_add_timer`), close reasons, and the
  signals. It knows no fonts, layout or windows.
- **The store's record**, a struct in awl_host.h the library only reads:
  id, app name, summary, body (raw markup; the library parses it), the
  image source (bytes with w/h/stride/alpha, or a path or name), the
  actions (key, label pairs), urgency, `value` (−1 none), flags (transient,
  resident), the timeout in ms, and a generation that grows on every
  change, so the library can tell "replaced" from "unchanged".
- **Host table** (the 0c/0d ABI bump, 31):
  - `notif_list(out, max)`: the open records, newest first; pointers valid
    until the next change.
  - `notif_close(id, reason)`: dismiss (reason 2), also from the library's
    actions.
  - `notif_invoke(id, key)`: `ActionInvoked` (plus `ActivationToken` when
    there is one), then closes unless resident.
  - `notif_hold(id, held)`: pauses the expiry while the pointer is over it;
    the timer lives in the core, the hover in the library.
- **Hook**: `notifications_changed()` in the plugin's hook table, called
  after every Notify, close and expiry (coalesced to one per loop
  iteration). The library diffs by id and generation: new windows, redraws,
  re-stacking, hides. After a reload the library starts from
  `notif_list()`, so nothing is lost.
- **Without the library** (not loaded, or failed): the core still owns the
  name and answers, with no windows; notifications expire as usual. That
  keeps clients that block on `Notify` from hanging.
- Do-not-disturb lives in the core (a cfgdict key), so it holds across
  reloads: `Notify` still stores and answers, the hook isn't called, and
  critical ones still show.
- Image decoding stays in the library (it is drawing, per scale), on its
  worker; the core only validates and copies.

### Spec coverage

- Methods: `Notify`, `CloseNotification`, `GetCapabilities` (body,
  body-markup, actions, icon-static), `GetServerInformation`. Not
  body-hyperlinks or body-images: `<a>` and `<img>` are reduced to text.
- Signals: `NotificationClosed` (reasons 1–4), `ActionInvoked`,
  `ActivationToken`.
- The name requested with `SD_BUS_NAME_QUEUE`: a running fnott keeps it if it
  was first, which is logged.

### Hints

- urgency.
- `transient`, `resident`, `category`.
- Replace in place: `x-canonical-private-synchronous`, `x-dunst-stack-tag`.
- Images and `value`: their own sections below.

### Images

One image per notification, the first of these that yields one (the spec's
order):

1. `image-data` (also the older `image_data`, `icon_data`): `(iiibiiay)`,
   width, height, rowstride, has_alpha, bits_per_sample, channels, data.
   Only 8 bits and 3 or 4 channels (matching has_alpha) are taken; the data
   must hold `(h - 1) * rowstride + w * channels` bytes; more than
   `AWL_ICON_MAX_DIM` wide or high is refused. A new
   `awl_icon_from_rgba()` in icons/icon.c, next to
   `awl_icon_from_pixmaps()`: RGB(A) to premultiplied ARGB, fitted the same
   way.
2. `image-path`, then `app_icon`: an absolute path, a `file://` URL
   (percent-decoded), else an icon name through `awl_icons_lookup()` with
   the desktop's themes.
3. `desktop-entry`: its Icon through the launcher's index (shared code).

- Formats: what `awl_icon_decode()` takes (PNG, SVG), plus JPEG, which
  `image-path` brings in practice (album art, screenshots).
  `awl_icon_decode()` goes by magic bytes instead of the svg flag. Other
  formats show no image.
- JPEG decoder (researched 2026-10-21): **Wuffs v0.4's `std/jpeg`**.
  - Its bounds and overflow checks are proved by the Wuffs compiler, it is
    fuzzed on OSS-Fuzz, it has no known CVEs, and it never allocates: the
    size (`decode_image_config`) and the work buffer (`workbuf_len`) are
    known first, so both are capped before any malloc (a progressive
    JPEG's worst-case work buffer is huge: cap it at a few MB).
  - Baseline, progressive, CMYK/YCCK; no arithmetic coding, lossless or
    12-bit (rare in practice).
  - Catch: v0.4 is still an alpha (0.4.0-alpha.12 on 2026-10-20; v0.3.5,
    vendored, has no JPEG). Vendoring it: `release/c/wuffs-v0.4.c` pinned
    to a commit, `WUFFS_CONFIG__MODULE__JPEG` in vendor/wuffs/wuffs.h. The
    one API change for the PNG code: `set_quirk_enabled(…, true)` becomes
    `set_quirk(…, 1)` (icons/icon.c, plugins/wallpaper.c); the rest is the
    same.
  - Not stb_image, although plutovg already builds it with JPEG and
    `STBI_MAX_DIMENSIONS 1024` (vendor/plutosvg/svg.c): several JPEG heap
    overflow CVEs (2021–2022), not on OSS-Fuzz, fixes left to distros.
  - Not libjpeg-turbo: fuzzed and complete, but memory-unsafe C with a
    CVE history, and a new system dependency, inside the compositor.
  - Taken: the pinned alpha, for PNG too (0.3 and 0.4 can't be linked
    side by side: the same symbols). Move to v0.4.0 once it is out.
- The store keeps the source, not pixels: image-data's bytes (copied; they
  are capped at `AWL_ICON_MAX_DIM`² · 4) or the path or name. The renderer
  decodes at the monitor's scale, so moving to another output stays sharp.
- Decoding: image-data on the main thread (a copy and a scale). Files and
  icon names on a worker (plugins/thread.h, as the launcher's icons), and
  the notification shows only once its image is done or failed, bounded by
  ~100 ms, so it doesn't change size right after it appears.
- Drawn left of the text at `image_size` (default 48 logical px), aspect
  kept, centered in that square; symbolic icons in the text color, as in
  the tray. Without an image the text starts at the padding.

### Markup

The body may have markup, the summary not (per the spec); the summary is
drawn bold.

- A parser of its own, plugins/markup.{c,h}, unit tested: text to runs of
  `{ text, style, color }`, style a mask of bold, italic, underline.
- Tags: `<b>`, `<i>`, `<u>`, and `<t color="…">` (awl's own, color as
  `#rgb`, `#rrggbb` or `#rrggbbaa`; an invalid color keeps the outer one).
  They nest through a small stack; a close tag closes up to its open tag.
  `<a href>` keeps its text, `<img alt>` becomes its alt text, `<br>` and
  `<br/>` a line break; any other well-formed tag is dropped with its text
  kept.
- Entities: `&amp; &lt; &gt; &quot; &apos;`, `&#N;`, `&#xN;`.
- Clients that don't escape are common, so: a `<` that doesn't start a
  well-formed tag is text, an unknown entity stays as it is, unclosed tags
  end with the text. Never a failure, never an empty body.
- Bold and italic come from 0d's `font_style`; underline is a line under
  the run (pixman fill, the font's `underline` position and thickness).

### Layout

In the library, plugins/notify_layout.{c,h} or part of markup.c, unit tested
against a fake `text_width`:

- Word wrap across runs: words measured in their run's font, a word may
  span runs (`a<b>b</b>c` is one word); breaks at spaces and after `-`, a
  word longer than the line breaks between characters. `\n` breaks.
- At most `max_lines` lines (config), the last cut with an ellipsis.
- Runs on a line share a baseline: each is drawn with `text_over` at
  `baseline - font->ascent`, its h the font's height. The line is as high
  as its highest font. Kerning stops at run boundaries.
- The layout is cached per notification and font, redone when the font
  (scale) or the text changes.

### Progress bar

- The `value` hint, 0–100 (int, uint or byte, as clients differ),
  clamped. Present: a bar under the body, full width of the text column,
  `progress_height` high, rounded: a track in the dim color, the filled part
  in `progress_color` (0: the urgency's frame color).
- Volume and brightness OSDs replace their notification several times a
  second: a replacement with the same layout only redraws (no
  re-stacking, no move), and restarts the timer.

### Behavior

- Default timeouts per urgency from config; critical never expires; the timer
  pauses while the pointer is over the notification.

### Interface

- One persistent window per notification, stacked from a corner (default top
  right, under the bar), at most N shown plus a "+k more" row.
- Image, bold summary, wrapped body with markup and a line limit, the
  progress bar, actions as clickable parts.
- Config, `awl_plugin_config_t.notify`: at, dx/dy, gap, width, image_size,
  max_lines, max_shown, timeouts per urgency, colors per urgency (fg, bg,
  frame), progress_color, progress_height; colors 0 = the popup's. Blur
  through a new `blur.notify`.
- Left click: the default action, else dismiss; right click: dismiss; middle
  click: dismiss all.
- Hidden while the screen is locked, shown again after.

### Actions and widget

- Actions: dismiss, dismiss all, the newest one's default action, show its
  actions (replacing `fnottctl actions` on MOD+Shift+N).
- Do-not-disturb: the core's cfgdict key, plus an optional bar widget with a
  count.
- Later: a history popup of the last N closed.

## Phase 3: tests

- Launcher: `live.sh key` (wtype) against a fake `$XDG_DATA_DIRS` with a few
  .desktop files; ranking, Exec expansion, the argv spawned.
- Notifications: test/windows.sh-style checks driven by `notify-send` and
  `gdbus`: replaces_id, actions with `ActionInvoked`, image-data, expiry,
  close reasons, behavior across a plugin reload; markup, images (PNG,
  JPEG, image-data, an icon name) and a `value` hint by screenshot.
- `make check`: fzy scoring and Exec parsing as unit tests; the markup
  parser (malformed input too, and a fuzz run), the wrap, the styled font
  lookups, `awl_icon_from_rgba()` with bad rowstrides and sizes.

## Order

1. 0a plus the tray menus' keyboard navigation (small, de-risks the grab).
2. 0b and 0c.
3. The launcher.
4. 0d (font styles), the markup parser and layout, JPEG (done);
   `awl_icon_from_rgba()`: standalone and unit tested.
5. The notification daemon (the biggest piece, for the spec's breadth), on
   top of 4: first the core's server and store with the host calls, tested
   by `gdbus` alone, then the library's windows.

## The user's config afterwards

plugins_config.c changes in three places, reported rather than made silently:
`menucmd` becomes the `launcher` action, `fnott` leaves autostart, `fnottctl`
is replaced by the new actions.
