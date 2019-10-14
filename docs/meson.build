asciidoc_files = ['spice_for_newbies',
                  'spice_protocol',
                  'spice_style',
                  'spice_threading_model',
                  'vd_interfaces']

if get_option('manual')
  asciidoc = find_program('asciidoc', required : false)
  if asciidoc.found()
    asciidoc_args = ['-a', 'data-uri', '-a', 'icons', '-a', 'toc']
    foreach doc : asciidoc_files
      custom_target('@0@.html'.format(doc),
                    input : '@0@.txt'.format(doc),
                    output : '@0@.html'.format(doc),
                    build_by_default: true,
                    command : [asciidoc, '-n', asciidoc_args, '-o', '@OUTPUT@', '@INPUT@'])
    endforeach
  endif
  subdir('manual')
endif
