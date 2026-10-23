# awl, libawltray-env.so and libawlplugins.so:
#
#   meson setup build                  once; optimized (release, LTO)
#   ninja -C build                     then after every change
#   meson test -C build                the unit tests (make check before)
#
#   meson setup build-dev --buildtype=debug -Db_lto=false
#                                      unoptimized with full debug info, for
#                                      test/live.sh's gdb (LIVE_AWL=build-dev/awl)
#
# Switches are in meson.options. Custom flags: the CUSTOM lists below, or
# -Dc_args=... / -Dc_link_args=... on the command line.
project('awl', 'c',
  version: '0.8-dev',
  meson_version: '>=1.1',
  default_options: [
    # awl itself only uses C99 features, but wlroots' headers use anonymous
    # unions (C11)
    'c_std=c11',
    'warning_level=3',
    'buildtype=release',
    'b_lto=true',
  ],
)

cc = meson.get_compiler('c')
# the sources' directory, for test/'s #include "awl_*.h"
root_inc = include_directories('.')
fs = import('fs')
dev = get_option('buildtype') == 'debug'

# CUSTOM: flags of your own, for everything (awl, the plugins, the tests);
# e.g. '-Wshadow', '-Wdeclaration-after-statement', '-Wfloat-conversion'
custom_c_args = []
custom_link_args = []

add_project_arguments('-Wno-unused-parameter', custom_c_args, language: 'c')
add_project_link_arguments(custom_link_args, language: 'c')
if dev
  add_project_arguments('-ggdb3', '-fno-omit-frame-pointer', language: 'c')
endif
# for compiling and, with LTO, for the link, which compiles again
opt_args = []
if get_option('native').disable_auto_if(dev).allowed()
  opt_args += ['-march=native', '-mtune=native']
endif
if get_option('fast_math').disable_auto_if(dev).allowed()
  opt_args += '-ffast-math'
endif
add_project_arguments(opt_args, language: 'c')
add_project_link_arguments(opt_args, language: 'c')
if get_option('b_lto')
  # LTO's false positives
  add_project_link_arguments('-Wno-maybe-uninitialized', language: 'c')
endif

# awl's and the plugins' own; not libawltray-env.so's
awl_c_args = ['-DWLR_USE_UNSTABLE', '-D_POSIX_C_SOURCE=200809L']
if get_option('scenefx_shatter')
  awl_c_args += '-DSCENEFX_HAS_SHATTER'
endif

wlroots   = dependency('wlroots-0.20')
wayland   = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
libinput  = dependency('libinput')
pixman    = dependency('pixman-1')
fcft      = dependency('fcft')
libsystemd = dependency('libsystemd')
libpulse  = dependency('libpulse')
threads   = dependency('threads')
libm      = cc.find_library('m')

xcb = dependency('xcb', required: get_option('xwayland'))
xcb_icccm = dependency('xcb-icccm', required: get_option('xwayland'))
xwayland = xcb.found() and xcb_icccm.found()
if xwayland
  awl_c_args += '-DXWAYLAND'
endif

# scenefx: from awl_libs (meson.options), found at run time there too, or
# through pkg-config
awl_libs = get_option('awl_libs')
if awl_libs != ''
  awl_libs = fs.expanduser(awl_libs)
  scenefx_lib = cc.find_library('scenefx-0.5', dirs: awl_libs / 'lib')
  scenefx = declare_dependency(
    dependencies: scenefx_lib,
    compile_args: '-I' + awl_libs / 'include' / 'scenefx-0.5',
    link_args: '-Wl,-rpath=' + awl_libs / 'lib',
  )
else
  scenefx = dependency('scenefx-0.5')
endif

# wayland-scanner generates C headers and rigging for Wayland protocols,
# which are specified in XML; awl.h includes them
wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
  native: true)
wl_protocols = dependency('wayland-protocols').get_variable('pkgdatadir')
protocol_headers = []
foreach p : [
  ['cursor-shape-v1', wl_protocols / 'staging/cursor-shape/cursor-shape-v1.xml', 'enum-header'],
  ['pointer-constraints-unstable-v1',
   wl_protocols / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml', 'enum-header'],
  ['wlr-layer-shell-unstable-v1',
   meson.project_source_root() / 'protocols/wlr-layer-shell-unstable-v1.xml', 'enum-header'],
  ['wlr-output-power-management-unstable-v1',
   meson.project_source_root() / 'protocols/wlr-output-power-management-unstable-v1.xml',
   'server-header'],
  ['xdg-shell', wl_protocols / 'stable/xdg-shell/xdg-shell.xml', 'server-header'],
]
  protocol_headers += custom_target(p[0] + '-protocol.h',
    input: p[1],
    output: p[0] + '-protocol.h',
    command: [wayland_scanner, p[2], '@INPUT@', '@OUTPUT@'],
  )
endforeach

version_h = vcs_tag(input: 'version.h.in', output: 'version.h',
  command: ['git', '-C', meson.project_source_root(), 'describe', '--tags', '--dirty'],
  fallback: meson.project_version())

# preloaded into the programs awl starts with TRAY_ICONS (tray/app_env.c,
# tray/env_shim.c), found next to awl; libc only
shared_library('awltray-env', 'tray/env_shim.c',
  dependencies: cc.find_library('dl', required: false),
  install: true,
)

# awl.c starts and ends awl; each awl_*.c is a part of it, see its header.
# scenefx first: it defines wlr_scene_* too, with its own struct layouts, and
# the dynamic linker binds each symbol to the first library that has it, so
# with wlroots ahead awl's scene would be wlroots' (no blur, no effects)
awl_deps = [scenefx, wlroots, wayland, xkbcommon, libinput, pixman, fcft, libsystemd, libm]
if xwayland
  awl_deps += [xcb, xcb_icccm]
endif
awl = executable('awl',
  'awl.c', 'awl_config.c', 'awl_output.c', 'awl_bar.c', 'awl_popup.c', 'awl_client.c',
  'awl_xwayland.c', 'awl_layout.c', 'awl_layer.c', 'awl_input.c', 'awl_cursor.c',
  'awl_lockscreen.c', 'awl_spawn.c', 'awl_wallpaper.c', 'awl_test.c', 'awl_log.c',
  'awl_dict.c', 'awl_util.c', 'awl_draw.c', 'awl_plugin_host.c', 'awl_desktop.c',
  'awl_background.c', 'awl_redraw.c', 'awl_sni.c', 'awl_bus.c', 'awl_notifications.c', 'awl_alert.c', 'awl_window.c',
  'awl_window_draw.c', 'tray/app_env.c',
  protocol_headers, version_h,
  c_args: awl_c_args,
  dependencies: awl_deps,
  install: true,
)

# libawlplugins.so: the plugin threads and the bar widgets, dlopen()ed by
# awl_plugin_host.c and reloaded with plugin_restart (see awl_plugin_abi.h).
# Only awl_plugin_entry is exported; everything else binds within the
# library. AWL_HOSTED: reaches awl only through awl_host (awl_host.h). The
# headers' cflags of awl's dependencies, not their libraries: those are awl's.
plugins_c_args = awl_c_args + ['-DAWL_HOSTED']
plugins_deps = [
  libpulse, libsystemd, pixman, fcft, threads, libm,
  wlroots.partial_dependency(compile_args: true, includes: true),
  wayland.partial_dependency(compile_args: true, includes: true),
  xkbcommon.partial_dependency(compile_args: true, includes: true),
  libinput.partial_dependency(compile_args: true, includes: true),
  scenefx.partial_dependency(compile_args: true, includes: true),
]
if xwayland
  plugins_deps += [xcb.partial_dependency(compile_args: true, includes: true),
                   xcb_icccm.partial_dependency(compile_args: true, includes: true)]
endif
plutovg_inc = include_directories('vendor/plutosvg/plutovg-1.3.3/include')

# vendor/plutosvg/svg.h, the SVGs of icons/icon.c: plutovg's header by <>,
# and IEEE floats for untrusted numbers (see vendor/plutosvg/svg.c)
plutosvg = static_library('plutosvg', 'vendor/plutosvg/svg.c',
  c_args: plugins_c_args + ['-fno-fast-math'],
  include_directories: plutovg_inc,
  dependencies: plugins_deps,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)
# ecal_check (meson.options): cal_eds.c against libecal's headers
cal_eds_c_args = plugins_c_args
cal_eds_deps = plugins_deps
if get_option('ecal_check')
  cal_eds_c_args += ['-DCAL_ECAL_CHECK', '-Wno-pedantic']
  cal_eds_deps += dependency('libecal-2.0').partial_dependency(compile_args: true, includes: true)
endif
cal_eds = static_library('cal_eds', 'plugins/cal_eds.c',
  c_args: cal_eds_c_args,
  dependencies: cal_eds_deps,
  pic: true,
  gnu_symbol_visibility: 'hidden',
)

plugins_git_h = custom_target('plugins_git.h',
  output: 'plugins_git.h',
  command: [find_program('build-aux/plugins_git.sh'), meson.project_source_root(), '@OUTPUT@'],
  build_always_stale: true,
)

plugins_sources = files(
  'plugins.c', 'plugins_widgets.c', 'plugins_desktop_panel.c', 'plugins_config.c',
  'plugins/backlight.c', 'plugins/bat.c', 'plugins/calendar.c', 'plugins/calpopup.c',
  'plugins/colors.c', 'plugins/date.c', 'plugins/desktop_entry.c', 'plugins/fuzzy.c', 'plugins/markup.c',
  'plugins/ipaddr.c', 'plugins/launcher.c', 'plugins/logind.c', 'plugins/loop.c',
  'plugins/nm.c', 'plugins/notify.c', 'plugins/notify_image.c', 'plugins/notify_menu.c', 'plugins/osd.c', 'plugins/persistent.c', 'plugins/poller.c', 'plugins/pulsetest.c',
  'plugins/readdir.c', 'plugins/sni.c', 'plugins/sni_menu.c', 'plugins/stats.c',
  'plugins/temp.c', 'plugins/thread.c', 'plugins/tray_menu_window.c',
  'plugins/tray_tooltip.c', 'plugins/wallpaper.c',
  'vendor/wuffs/wuffs.c', 'icons/icon.c',
)
# linked under a temporary name and copied into place by a rename, so a
# running awl that reloads mid-build reads either the old or the new library,
# never half of one
plugins_linked = shared_library('awlplugins-linked',
  plugins_sources, protocol_headers, plugins_git_h,
  c_args: plugins_c_args,
  include_directories: plutovg_inc,
  dependencies: plugins_deps,
  link_whole: [plutosvg, cal_eds],
  gnu_symbol_visibility: 'hidden',
)
custom_target('libawlplugins.so',
  input: plugins_linked,
  output: 'libawlplugins.so',
  command: [find_program('sh'), '-c', 'cp -f "$1" "$2.tmp" && mv -f "$2.tmp" "$2"',
            'sh', '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
  install: true,
  install_dir: 'lib/',
)

subdir('test')
