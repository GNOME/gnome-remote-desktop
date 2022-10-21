a2x = find_program('a2x')

attributes = []
if have_rdp
  attributes += ['-a have_rdp']
endif
if have_vnc
  attributes += ['-a have_vnc']
endif

custom_target('man page',
  input: ['grdctl.txt', 'stylesheet.xsl'],
  output: 'grdctl.1',
  command: [a2x, '-v', '-D', '@OUTDIR@', '--xsl-file', '@INPUT1@', '-f' ,'manpage', '@INPUT0@', attributes],
  install_dir: mandir + '/man1',
  install: true
)
