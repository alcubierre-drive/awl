# awlock

awl's screen locker, part of awl: it locks awl over ext-session-lock-v1
like any locker, but it is no program of its own. At its start, before any
thread or device, awl forks a lock helper (awl_lockhelper.c, shown as
`awl-lockhelper`); for each lock, awl sends it the config's `locker`
arguments and a socket for the Wayland connection, and the helper forks
awlock (shown as `awlock`). A locker that crashes takes only itself, and
awl stays locked until the next one, which awl starts. awlock can't be
core dumped or traced (it holds the password), and it logs into awl's log,
at awl's level (`-d` in `locker`: everything).

Instead of a ring it draws
awl's logo (logo.xpm, compiled in) at the pointer, coloured by state: waiting, typing,
Caps Lock, validating, wrong. On outputs without the pointer the logo is
centred and dimmed. `--logo FILE` draws another XPM instead, of the same
kind as logo.xpm: up to 256 by 256, one character per pixel, the colours
`None` and two `#rrggbb` (the darker is the border, the lighter the fill;
the states' colours replace both). A file awlock can't use is logged, and
it locks with logo.xpm. The options are in awlock.c's `usage`; `-h` in
`locker` prints them to awl's log (and doesn't lock).

It is a fork of swaylock-fprintd (https://github.com/SL-RU/swaylock-fprintd,
536d9df), itself a fork of swaylock (https://github.com/swaywm/swaylock),
copied in and built from awl's meson.build. Passwords go through PAM's
`system-auth` (meson option `lock_pam_service`), fingerprints through
fprintd on the system bus (fingerprint.c, sd-bus), if there is a reader.

License: GPL-3.0-or-later, as awl (../LICENSE). It contains code from
swaylock, used under its MIT license, which ../LICENSE reproduces.
fingerprint.c is new; upstream's GPL-2.0-or-later fingerprint code is gone.
