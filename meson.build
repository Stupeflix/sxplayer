project(
  'sxplayer',
  'c',
  default_options: ['c_std=c99'],
  license: 'LGPL-2.1',
  meson_version: '>= 0.49.0',
  # We don't use `cat` here for portability concerns. The VERSION file is
  # managed by the `make-release.sh` script
  # See https://github.com/mesonbuild/meson/issues/7890 for more information
  version: run_command(
    find_program('python'), '-c', 'import sys;print(open(sys.argv[1]).read())', files('VERSION')
  ).stdout().strip(),
)

conf_data = configuration_data()
version_array = meson.project_version().split('.')
conf_data.set('version_major', version_array[0])
conf_data.set('version_minor', version_array[1])
conf_data.set('version_micro', version_array[2])

host_system = host_machine.system()
cc = meson.get_compiler('c')

install_rpath = get_option('rpath') ? get_option('prefix') / get_option('libdir') : ''

# This trim prefix is used to make __FILE__ starts from the source dir with
# out-of-tree builds.
# Adjusted from https://github.com/mesonbuild/meson/issues/7485
trim_prefix = run_command([
  find_program('python'), '-c', 'import sys,os;print(os.path.relpath(*sys.argv[1:3]))',
  meson.current_source_dir(),
  meson.build_root(),
]).stdout().strip()

add_project_arguments(
  cc.get_supported_arguments([
    '-Werror=missing-prototypes',
    '-fmacro-prefix-map=@0@/='.format(trim_prefix),
  ]),
  language: 'c',
)

if get_option('trace')
  add_project_arguments('-DENABLE_DBG=1', language: 'c')
endif


#
# Dependencies, sometimes shared with the tools and tests
#

lib_deps = [
  cc.find_library('m', required: false),
  dependency('threads'),
  dependency('libavformat', version: '>= 58.12.100'),
  dependency('libavfilter', version: '>= 7.16.100'),
  dependency('libavcodec', version: '>= 58.18.100'),
  dependency('libavutil', version: '>= 56.14.100'),
]

vaapi_dep = dependency('libva', version: '>= 1.1.0', required: get_option('vaapi'))
if vaapi_dep.found()
  lib_deps += vaapi_dep
  add_project_arguments('-DHAVE_VAAPI_HWACCEL=1', language: 'c')
endif

if host_system == 'darwin'
  lib_deps += dependency('appleframeworks', modules: ['CoreFoundation', 'VideoToolbox', 'CoreMedia', 'QuartzCore'])
endif


#
# Library
#

lib_src = files(
  'async.c',
  'decoder_ffmpeg.c',
  'decoders.c',
  'log.c',
  'mod_decoding.c',
  'mod_demuxing.c',
  'mod_filtering.c',
  'msg.c',
  'sxplayer.c',
  'utils.c',
)

lib_link_args = cc.get_supported_link_arguments([
  '-Wl,--version-script,@0@'.format(meson.current_source_dir() / 'libsxplayer.symexport'),  # GNU ld
  '-Wl,-exported_symbols_list,@0@'.format(meson.current_source_dir() / 'libsxplayer.darwin.symexport'),  # Darwin
])

if host_system == 'darwin'
  lib_src += files('decoder_vt.c')
endif

libsxplayer = library(
  'sxplayer',
  lib_src,
  dependencies: lib_deps,
  install: true,
  install_rpath: install_rpath,
  version: meson.project_version(),
  link_args: lib_link_args,
)

lib_header = configure_file(
  input: 'sxplayer.h.in',
  output: 'sxplayer.h',
  configuration: conf_data
)
install_headers(lib_header)
if get_option('cpp-header')
  install_headers(files('sxplayer.hpp'))
endif

pkg = import('pkgconfig')
pkg.generate(
  libsxplayer,
  name: 'libsxplayer',  # not specifying the name would fallback on "sxplayer.pc" instead of "libsxplayer.pc"
  description: 'Stupeflix Player library',
)


#
# Player
#

player_deps = lib_deps + [
  dependency('glfw3', required: get_option('player')),
  dependency('glew', required: get_option('player')),
]
if host_system == 'darwin'
  player_deps += dependency('appleframeworks', modules: ['OpenGL'], required: get_option('player'))
endif
player_deps_found = true
foreach dep : player_deps
  player_deps_found = player_deps_found and dep.found()
endforeach
if player_deps_found
  player = executable(
    'sxplayer',
    files('player.c'),
    dependencies: player_deps,
    link_with: libsxplayer,
    install: true,
    install_rpath: install_rpath,
  )
endif


#
# Tests
#

if get_option('tests')
  test_prog = executable(
    'test-prog',
    files('test-prog.c'),
    dependencies: lib_deps,
    link_with: libsxplayer,
    install: false,
  )
  test(
    'all',
    test_prog,
    args: files('media.mkv', 'image.jpg'),
    timeout: 24*60*60,
  )
endif
