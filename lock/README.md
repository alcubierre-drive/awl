# awlock

awl's screen locker: a separate program that locks awl over
ext-session-lock-v1, started as awl's `locker`. Instead of a ring it draws
awl's logo (logo.xpm, compiled in) at the pointer, coloured by state: waiting, typing,
Caps Lock, validating, wrong. On outputs without the pointer the logo is
centred and dimmed. `awlock -h` lists the options.

It is a fork of swaylock-fprintd (https://github.com/SL-RU/swaylock-fprintd,
536d9df), itself a fork of swaylock (https://github.com/swaywm/swaylock),
copied in and built from awl's meson.build. Passwords go through PAM's
`system-auth` (meson option `lock_pam_service`), fingerprints through
fprintd on the system bus (fingerprint.c, sd-bus), if there is a reader.

License: GPL-3.0-or-later, as awl (../LICENSE). It contains code from
swaylock, used under its MIT license, which ../LICENSE reproduces.
fingerprint.c is new; upstream's GPL-2.0-or-later fingerprint code is gone.
