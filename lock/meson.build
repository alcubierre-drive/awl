# awlock: the lock screen client, a fork of swaylock-fprintd
# (https://github.com/SL-RU/swaylock-fprintd, 536d9df), copied in. A separate
# program, not part of awl: it talks to awl over ext-session-lock-v1 like any
# other locker. See README.md.
#
# Built from awl's meson.build (subdir), with swaylock's own warnings: awl's
# warning_level=3 (-Wpedantic) is not applied to it.
lock_wayland_client = dependency('wayland-client', version: '>=1.20.0', required: get_option('lock'))
# passwords go through PAM only (auth.c), as the user: no shadow fallback
lock_pam = cc.find_library('pam', required: get_option('lock'))

if lock_wayland_client.found() and lock_pam.found()

  lock_protos = []
  # the lock; the transparent background: one pixel, stretched
  foreach xml : [wl_protocols / 'staging/ext-session-lock/ext-session-lock-v1.xml',
                 wl_protocols / 'staging/single-pixel-buffer/single-pixel-buffer-v1.xml',
                 wl_protocols / 'stable/viewporter/viewporter.xml']
    base = fs.stem(xml)
    lock_protos += custom_target(base + '-protocol.c', input: xml,
      output: base + '-protocol.c',
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
    lock_protos += custom_target(base + '-client-protocol.h', input: xml,
      output: base + '-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
  endforeach

  # libsystemd (or libelogind): sd-bus, for fprintd (fingerprint.c)
  lock_deps = [libm, xkbcommon, lock_wayland_client, libsystemd, lock_pam]
  # the Wayland side; passwords and PAM; fprintd (awlock.h)
  lock_sources = files('awlock.c', 'auth.c', 'fingerprint.c')

  lock_c_args = ['-Wno-unused-result', '-Wundef', '-Wvla',
    '-DAWLOCK_VERSION="@0@"'.format(meson.project_version()),
    '-DLOCK_PAM_SERVICE="@0@"'.format(get_option('lock_pam_service'))]
  # the executable itself is in awl's meson.build: next to awl in the build
  # directory too, where awl looks for its locker first
  # PAM: an existing service (lock_pam_service, auth.c), so awlock needs no
  # file of its own in /etc/pam.d
endif
