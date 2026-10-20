# meson test: test/window_unit.c, awl_window_draw.c without a compositor
# (test/windows.sh tests the popups in a running awl), test/fuzzy_unit.c and
# test/desktop_entry_unit.c, the launcher's matcher and .desktop files. Each
# exits with the number of failed checks.
window_unit = executable('window_unit', 'window_unit.c', protocol_headers,
  objects: awl.extract_objects('awl_window_draw.c', 'awl_draw.c', 'awl_log.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
  dependencies: awl_deps,
)
fuzzy_unit = executable('fuzzy_unit', 'fuzzy_unit.c',
  objects: plugins_linked.extract_objects('plugins/fuzzy.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
)
desktop_entry_unit = executable('desktop_entry_unit', 'desktop_entry_unit.c',
  objects: plugins_linked.extract_objects('plugins/desktop_entry.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
)
test('window_unit', window_unit)
test('fuzzy_unit', fuzzy_unit)
test('desktop_entry_unit', desktop_entry_unit)

# vptr, the pointer injector live.sh uses; sni_menu, a tray item with a menu
vptr_protocol = 'wlr-virtual-pointer-unstable-v1.xml'
executable('vptr', 'vptr.c',
  custom_target('wlr-virtual-pointer-unstable-v1-client-protocol.h',
    input: vptr_protocol, output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('wlr-virtual-pointer-unstable-v1-protocol.c',
    input: vptr_protocol, output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
  dependencies: dependency('wayland-client'),
)
gio = dependency('gio-2.0', required: false)
gdk_pixbuf = dependency('gdk-pixbuf-2.0', required: false)
if gio.found() and gdk_pixbuf.found()
  executable('sni_menu', 'sni_menu.c', dependencies: [gio, gdk_pixbuf])
endif
