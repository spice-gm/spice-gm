spice_server_include += [include_directories('.')]

#
# generate spice-version.h
#
if meson.project_version().startswith('UNKNOWN')
  major = '0'
  minor = '0'
  micro = '1'
else
  version_info = meson.project_version().split('.')
  major = '@0@'.format(version_info[0])
  minor = '@0@'.format(version_info[1])
  micro = version_info[2].to_int()
  if version_info.length() >= 4
    micro += 1
  endif
  micro = '@0@'.format(micro)
endif

cmd = run_command('printf', ['0x%02x%02x%02x', major, minor, micro], check : true)
version_data = configuration_data()
version_data.set('SPICE_SERVER_VERSION', cmd.stdout().strip())
spice_version_h = configure_file(input : 'spice-version.h.in',
                                 output : 'spice-version.h',
                                 configuration : version_data)

#
# libspice-server.so
#
spice_server_headers = [
  spice_version_h,
  'spice-audio.h',
  'spice-char.h',
  'spice-core.h',
  'spice-input.h',
  'spice-migration.h',
  'spice-qxl.h',
  'spice-server.h',
  'spice-replay.h',
  'spice.h',
]

install_headers(spice_server_headers, subdir : 'spice-server')

# generate enums
gnome = import('gnome')
spice_server_enums = gnome.mkenums('spice-server-enums',
                                   sources : 'spice-server.h',
                                   symbol_prefix : 'SPICE',
                                   c_template: 'spice-server-enums.c.tmpl',
                                   h_template: 'spice-server-enums.h.tmpl')

spice_server_sources = [
  spice_server_headers,
  spice_server_enums,
  'agent-msg-filter.c',
  'agent-msg-filter.h',
  'cache-item.h',
  'char-device.cpp',
  'char-device.h',
  'common-graphics-channel.cpp',
  'common-graphics-channel.h',
  'cursor-channel.cpp',
  'cursor-channel-client.cpp',
  'cursor-channel-client.h',
  'cursor-channel.h',
  'utils.hpp',
  'safe-list.hpp',
  'dcc.cpp',
  'dcc.h',
  'dcc-private.h',
  'dcc-send.cpp',
  'dispatcher.cpp',
  'dispatcher.h',
  'display-channel.cpp',
  'display-channel.h',
  'display-channel-private.h',
  'display-limits.h',
  'event-loop.c',
  'glz-encoder.c',
  'glz-encoder-dict.c',
  'glz-encoder-dict.h',
  'glz-encoder.h',
  'glz-encoder-priv.h',
  'image-cache.cpp',
  'image-cache.h',
  'image-encoders.cpp',
  'image-encoders.h',
  'inputs-channel.cpp',
  'inputs-channel-client.cpp',
  'inputs-channel-client.h',
  'inputs-channel.h',
  'jpeg-encoder.c',
  'jpeg-encoder.h',
  'main-channel.cpp',
  'main-channel-client.cpp',
  'main-channel-client.h',
  'main-channel.h',
  'main-dispatcher.cpp',
  'main-dispatcher.h',
  'memslot.c',
  'memslot.h',
  'migration-protocol.h',
  'mjpeg-encoder.c',
  'net-utils.c',
  'net-utils.h',
  'pixmap-cache.cpp',
  'pixmap-cache.h',
  'red-channel.cpp',
  'red-channel-capabilities.c',
  'red-channel-capabilities.h',
  'red-channel-client.cpp',
  'red-channel-client.h',
  'red-channel.h',
  'red-client.cpp',
  'red-client.h',
  'red-common.h',
  'red-parse-qxl.cpp',
  'red-parse-qxl.h',
  'red-pipe-item.cpp',
  'red-pipe-item.h',
  'red-qxl.cpp',
  'red-qxl.h',
  'red-record-qxl.c',
  'red-record-qxl.h',
  'red-replay-qxl.cpp',
  'reds.cpp',
  'reds.h',
  'reds-private.h',
  'red-stream.cpp',
  'red-stream.h',
  'red-worker.cpp',
  'red-worker.h',
  'sound.cpp',
  'sound.h',
  'spice-bitmap-utils.c',
  'spice-bitmap-utils.h',
  'spicevmc.cpp',
  'spice-wrapped.h',
  'stat-file.c',
  'stat-file.h',
  'stat.h',
  'stream-channel.cpp',
  'stream-channel.h',
  'sys-socket.c',
  'sys-socket.h',
  'red-stream-device.cpp',
  'red-stream-device.h',
  'sw-canvas.c',
  'tree.cpp',
  'tree.h',
  'utils.c',
  'utils.h',
  'video-encoder.h',
  'video-stream.cpp',
  'video-stream.h',
  'websocket.c',
  'websocket.h',
  'zlib-encoder.c',
  'zlib-encoder.h',
]

if spice_server_has_lz4 == true
  spice_server_sources += ['lz4-encoder.c',
                           'lz4-encoder.h']
endif

if spice_server_has_smartcard == true
  spice_server_sources += ['smartcard.cpp',
                           'smartcard.h',
                           'smartcard-channel-client.cpp',
                           'smartcard-channel-client.h']
endif

if spice_server_has_gstreamer == true
  spice_server_sources += ['gstreamer-encoder.c']
endif

#
# custom link_args
#
spice_server_syms = files('spice-server.syms')
spice_server_syms_path = join_paths(meson.current_source_dir(), 'spice-server.syms')
spice_server_link_args = ['-Wl,--no-copy-dt-needed-entries',
                          '-Wl,-z,relro',
                          '-Wl,-z,now',
                          '-Wl,--version-script=@0@'.format(spice_server_syms_path)]

spice_server_libs = library('spice-server', spice_server_sources,
                            version : spice_server_so_version,
                            install : true,
                            include_directories : spice_server_include,
                            link_args : compiler.get_supported_link_arguments(spice_server_link_args),
                            link_depends : spice_server_syms,
                            dependencies : spice_server_deps,
                            gnu_symbol_visibility : 'hidden')

if get_option('tests')
  if get_option('default_library') == 'both'
    spice_server_shared_lib = spice_server_libs.get_shared_lib()
    spice_server_static_lib = spice_server_libs.get_static_lib()
  elif get_option('default_library') == 'shared'
    spice_server_shared_lib = spice_server_libs
    # create static library extracting objects from shared one, avoid to
    # install static library using both_libraries
    spice_server_static_lib = static_library('spice-server-static',
                                             objects: spice_server_libs.extract_all_objects(),
                                             dependencies : spice_server_deps)
  else
    # here we use the static library to link to utilities (currently spice-server-replay)
    spice_server_shared_lib = spice_server_libs
    spice_server_static_lib = spice_server_libs
  endif
  subdir('tests')
endif

pc = import('pkgconfig')
pc.generate(spice_server_libs,
            description : 'SPICE server library',
            subdirs : 'spice-server',
            requires : 'spice-protocol')
