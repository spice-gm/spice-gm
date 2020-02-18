spice_server_include += [include_directories('.')]

#
# generate spice-version.h
#
version_info = meson.project_version().split('.')
major = '@0@'.format(version_info[0])
minor = '@0@'.format(version_info[1])
micro = version_info[2].to_int()
if not version_info.contains('git')
  micro += 1
endif
micro = '@0@'.format(micro)
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
spice_server_enums = gnome.mkenums_simple('spice-server-enums',
                                          sources : 'spice-server.h',
                                          symbol_prefix : 'SPICE')

spice_server_sources = [
  spice_server_headers,
  spice_server_enums,
  'agent-msg-filter.c',
  'agent-msg-filter.h',
  'cache-item.h',
  'char-device.c',
  'char-device.h',
  'common-graphics-channel.c',
  'common-graphics-channel.h',
  'cursor-channel.c',
  'cursor-channel-client.c',
  'cursor-channel-client.h',
  'cursor-channel.h',
  'dcc.c',
  'dcc.h',
  'dcc-private.h',
  'dcc-send.c',
  'dispatcher.c',
  'dispatcher.h',
  'display-channel.c',
  'display-channel.h',
  'display-channel-private.h',
  'display-limits.h',
  'event-loop.c',
  'glz-encoder.c',
  'glz-encoder-dict.c',
  'glz-encoder-dict.h',
  'glz-encoder.h',
  'glz-encoder-priv.h',
  'image-cache.c',
  'image-cache.h',
  'image-encoders.c',
  'image-encoders.h',
  'inputs-channel.c',
  'inputs-channel-client.c',
  'inputs-channel-client.h',
  'inputs-channel.h',
  'jpeg-encoder.c',
  'jpeg-encoder.h',
  'main-channel.c',
  'main-channel-client.c',
  'main-channel-client.h',
  'main-channel.h',
  'main-dispatcher.c',
  'main-dispatcher.h',
  'memslot.c',
  'memslot.h',
  'migration-protocol.h',
  'mjpeg-encoder.c',
  'net-utils.c',
  'net-utils.h',
  'pixmap-cache.c',
  'pixmap-cache.h',
  'red-channel.c',
  'red-channel-capabilities.c',
  'red-channel-capabilities.h',
  'red-channel-client.c',
  'red-channel-client.h',
  'red-channel.h',
  'red-client.c',
  'red-client.h',
  'red-common.h',
  'red-parse-qxl.c',
  'red-parse-qxl.h',
  'red-pipe-item.c',
  'red-pipe-item.h',
  'red-qxl.c',
  'red-qxl.h',
  'red-record-qxl.c',
  'red-record-qxl.h',
  'red-replay-qxl.c',
  'reds.c',
  'reds.h',
  'reds-private.h',
  'red-stream.c',
  'red-stream.h',
  'red-worker.c',
  'red-worker.h',
  'sound.c',
  'sound.h',
  'spice-bitmap-utils.c',
  'spice-bitmap-utils.h',
  'spicevmc.c',
  'stat-file.c',
  'stat-file.h',
  'stat.h',
  'stream-channel.c',
  'stream-channel.h',
  'sys-socket.c',
  'sys-socket.h',
  'red-stream-device.c',
  'red-stream-device.h',
  'sw-canvas.c',
  'tree.c',
  'tree.h',
  'utils.c',
  'utils.h',
  'video-encoder.h',
  'video-stream.c',
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
  spice_server_sources += ['smartcard.c',
                           'smartcard.h',
                           'smartcard-channel-client.c',
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

spice_server_libs = both_libraries('spice-server', spice_server_sources,
                                   version : spice_server_so_version,
                                   install : true,
                                   include_directories : spice_server_include,
                                   link_args : compiler.get_supported_link_arguments(spice_server_link_args),
                                   link_depends : spice_server_syms,
                                   dependencies : spice_server_deps,
                                   gnu_symbol_visibility : 'hidden')

spice_server_shared_lib = spice_server_libs.get_shared_lib()
spice_server_static_lib = spice_server_libs.get_static_lib()

subdir('tests')
