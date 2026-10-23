# Plan: awl's own notification looks

Everything else in the notification daemon is done (server, windows, images,
menu, history, DnD, idle pause, xdg activation). Left: proper styling for
awl's internal notifications. Not planned: desktop actions, activation tokens
for spawns, a launcher test.

## The senders (where to hook in)

| kind     | where                                                   | today                     |
|----------|---------------------------------------------------------|---------------------------|
| alert    | awl_alert.c `notify()` (core, critical)                 | `AWL_NOTIF_SEND_INIT`     |
| output   | awl_output.c `profilerefused()` (core)                  | `AWL_NOTIF_SEND_INIT`     |
| info     | plugins.c `awl_notify()`: wallpaper shown/timer, plugins_desktop_panel.c (desktop files, wallpaper info) | `AWL_NOTIF_SEND_INIT` |
| config   | plugins.c `awl_notify_config()`                         | `max_lines = -1` only     |
| osd      | plugins/osd.c `send()`                                  | `awl_plugin_config_t.osd` |

All of them build an `awl_notif_send_t`; the look is `awl_notif_look_t`
(awl_host.h), and plugins/notify.c already falls back field by field
(0 = `notify` config / urgency colors).

## Proposal

- One `awl_notif_look_t` per kind, configured in one place:
  `awl_config_t.notifications.looks.{alert,output,info,config,osd}`. It has to be
  the core config, because the core senders can't read `awl_plugin_config_t`;
  the library reads it through `awl_config()`. Defaults in
  awl_config_defaults.h (config: `max_lines = -1`; the rest 0).
- Each sender copies its look in place of `AWL_NOTIF_SEND_INIT`'s zero look.
  `awl_notify()` takes the kind (or `awl_notify_info()`/`_config()` wrappers).
- osd: move `osd.width`/`image_size` into `looks.osd`; the progress color
  rule (low fg, `bad` above 100%) stays in osd.c as the fallback when
  `looks.osd.progress` is 0.

## To consider

- `awl_notif_look_t` must move to (or be visible from) awl.h, since
  `awl_config_t` embeds it; awl_host.h keeps using it.
- Maybe extend the look: urgency, timeout and a default icon per kind
  (alerts carry no icon today). Decide before the ABI bump.
- ABI bump: `awl_config_t` layout changes (`sizeof_config` check).
- A tag replacement must take the new look; a history entry shown again
  (`notif_show`) should keep it. Check awl_notifications.c copies `look`
  into the past record.
- Alerts are critical: their frame comes from `notify_critical_*` unless the
  look sets one; keep the red default.
- The test FIFO `nsend` already takes `max_lines`/`width`, so no new test
  hooks are needed; looks are checked by eye.
