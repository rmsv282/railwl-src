project('railwl', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])

wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
libdrm = dependency('libdrm')
xkbcommon = dependency('xkbcommon')
math_dep = meson.get_compiler('c').find_library('m')
gbm = dependency('gbm')
egl = dependency('egl')
glesv2 = dependency('glesv2')

cc = meson.get_compiler('c')
libseat_dep = dependency('libseat', required: false)          # seatd: run as a normal user
if libseat_dep.found()
  add_project_arguments('-DHAVE_LIBSEAT=1', language: 'c')
endif
execinfo_dep = cc.find_library('execinfo', required: false)   # backtrace() lives here on FreeBSD
inotify_dep = dependency('libinotify', required: false)       # FreeBSD: devel/libinotify (if not native)

wlp_dir = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')

xdg_xml = wlp_dir / 'stable/xdg-shell/xdg-shell.xml'
xdg_scanner = find_program('wayland-scanner')

xdg_header = custom_target('xdg-shell-protocol.h',
  input: xdg_xml,
  output: 'xdg-shell-protocol.h',
  command: [xdg_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

xdg_code = custom_target('xdg-shell-protocol.c',
  input: xdg_xml,
  output: 'xdg-shell-protocol.c',
  command: [xdg_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

deco_xml = wlp_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
deco_header = custom_target('xdg-decoration-protocol.h',
  input: deco_xml,
  output: 'xdg-decoration-protocol.h',
  command: [xdg_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

deco_code = custom_target('xdg-decoration-protocol.c',
  input: deco_xml,
  output: 'xdg-decoration-protocol.c',
  command: [xdg_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

primary_xml = wlp_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml'
primary_header = custom_target('primary-selection-unstable-v1-protocol.h',
  input: primary_xml,
  output: 'primary-selection-unstable-v1-protocol.h',
  command: [xdg_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

primary_code = custom_target('primary-selection-unstable-v1-protocol.c',
  input: primary_xml,
  output: 'primary-selection-unstable-v1-protocol.c',
  command: [xdg_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

dmabuf_xml = wlp_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml'
dmabuf_header = custom_target('linux-dmabuf-v1-protocol.h',
  input: dmabuf_xml,
  output: 'linux-dmabuf-v1-protocol.h',
  command: [xdg_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

dmabuf_code = custom_target('linux-dmabuf-v1-protocol.c',
  input: dmabuf_xml,
  output: 'linux-dmabuf-v1-protocol.c',
  command: [xdg_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

xdg_client_header = custom_target('xdg-shell-client-protocol.h',
  input: xdg_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [xdg_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

primary_client_header = custom_target('primary-selection-unstable-v1-client-protocol.h',
  input: primary_xml,
  output: 'primary-selection-unstable-v1-client-protocol.h',
  command: [xdg_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

# --- screenshots: ext-image-copy-capture-v1 (+ the two protocols it refers to) ---
cap_gen = []
foreach name, xml : {
    'ext-foreign-toplevel-list-v1': wlp_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
    'ext-image-capture-source-v1':  wlp_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
    'ext-image-copy-capture-v1':    wlp_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  }
  cap_gen += custom_target(name + '-protocol.h',
    input: xml,
    output: name + '-protocol.h',
    command: [xdg_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
  cap_gen += custom_target(name + '-protocol.c',
    input: xml,
    output: name + '-protocol.c',
    command: [xdg_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
endforeach

# --- compositor ---
railwl_sources = files(
  'src/main.c',
  'src/compositor.c',
  'src/output.c',
  'src/input.c',
  'src/vt.c',
  'src/capture.c',
  'src/lseat.c',
  'src/trace.c',
)

executable('railwl', railwl_sources + [xdg_header, xdg_code, deco_header, deco_code,
                                       primary_header, primary_code,
                                       dmabuf_header, dmabuf_code] + cap_gen,
  include_directories: include_directories('include'),
  dependencies: [wayland_server, libdrm, xkbcommon, math_dep, gbm, egl, glesv2, execinfo_dep, libseat_dep],
  link_args: ['-rdynamic'])

# --- shared client lib ---
railclient_lib = static_library('railclient',
  ['src/client/railclient.c', 'src/client/font.c', xdg_code, xdg_client_header],
  include_directories: include_directories('include'),
  dependencies: [wayland_client, xkbcommon])

railclient_dep = declare_dependency(
  link_with: railclient_lib,
  include_directories: include_directories('include'),
  dependencies: [wayland_client, xkbcommon],
  sources: [xdg_client_header])

# --- utilities ---
executable('railbar', 'utils/railbar.c',
  dependencies: [railclient_dep, inotify_dep])

executable('raillauncher', 'utils/raillauncher.c',
  dependencies: railclient_dep)

executable('railnotify', 'utils/railnotify.c',
  dependencies: railclient_dep)

# --- key test ---
executable('key_test', 'test/key_test.c',
  include_directories: include_directories('include', 'src'),
  dependencies: [wayland_server, libdrm, xkbcommon])

# --- clipboard test: a from-scratch reader (the receiving half of the
#     two-client clipboard check, independent of wl-clipboard) ---
executable('clip_test', ['test/clip_test.c', primary_client_header, primary_code,
                         xdg_client_header, xdg_code],
  dependencies: [wayland_client])
