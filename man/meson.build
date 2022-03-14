a2x = find_program('a2x')

custom_target('man page',
  input: ['grdctl.txt', 'stylesheet.xsl'],
  output: 'grdctl.1',
  command: [a2x, '-v', '-D', '@OUTDIR@', '--xsl-file', '@INPUT1@', '-f' ,'manpage', '@INPUT0@'],
  install_dir: mandir + '/man1',
  install: true
)
