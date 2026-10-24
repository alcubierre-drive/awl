# meson test: test/window_unit.c, awl_window_draw.c without a compositor;
# test/font_unit.c, the font registry's styled fonts
# (test/windows.sh tests the popups in a running awl), test/fuzzy_unit.c and
# test/desktop_entry_unit.c, the launcher's matcher and .desktop files;
# test/markup_unit.c, notification markup and its layout; test/icon_unit.c,
# PNG, JPEG, GIF and XPM decoding (test/data). Each
# exits with the number of failed checks.
window_unit = executable('window_unit', 'window_unit.c', protocol_headers,
  objects: awl.extract_objects('awl_window_draw.c', 'awl_draw.c', 'awl_log.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
  dependencies: awl_deps,
)
font_unit = executable('font_unit', 'font_unit.c', protocol_headers,
  objects: awl.extract_objects('awl_draw.c', 'awl_log.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
  dependencies: awl_deps,
)
fuzzy_unit = executable('fuzzy_unit', 'fuzzy_unit.c',
  objects: plugins_linked.extract_objects('plugins/fuzzy.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
)
markup_unit = executable('markup_unit', 'markup_unit.c',
  objects: plugins_linked.extract_objects('plugins/markup.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
  dependencies: dependency('fcft'),
)
icon_unit = executable('icon_unit', 'icon_unit.c', protocol_headers,
  objects: plugins_linked.extract_objects('icons/icon.c', 'icons/xpm.c', 'vendor/wuffs/wuffs.c'),
  link_with: plutosvg,
  c_args: plugins_c_args,
  include_directories: [root_inc, plutovg_inc],
  dependencies: plugins_deps,
)
desktop_entry_unit = executable('desktop_entry_unit', 'desktop_entry_unit.c',
  objects: plugins_linked.extract_objects('plugins/desktop_entry.c'),
  c_args: awl_c_args,
  include_directories: root_inc,
)
test('window_unit', window_unit)
test('font_unit', font_unit)
test('fuzzy_unit', fuzzy_unit)
test('desktop_entry_unit', desktop_entry_unit)
test('markup_unit', markup_unit)
test('icon_unit', icon_unit, args: [meson.current_source_dir() / 'data'])

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
