if host_machine.system() != 'windows'
  executable('reds_stat', 'reds_stat.c',
             install : false,
             include_directories : spice_server_include,
             dependencies : spice_server_deps)
endif
