daemon_sources = files([
  'grd-context.c',
  'grd-context.h',
  'grd-daemon.c',
  'grd-daemon.h',
  'grd-pipewire-utils.c',
  'grd-pipewire-utils.h',
  'grd-private.h',
  'grd-prompt.c',
  'grd-prompt.h',
  'grd-session.c',
  'grd-session.h',
  'grd-session-vnc.c',
  'grd-session-vnc.h',
  'grd-settings.c',
  'grd-settings.h',
  'grd-stream.c',
  'grd-stream.h',
  'grd-types.h',
  'grd-vnc-cursor.c',
  'grd-vnc-cursor.h',
  'grd-vnc-pipewire-stream.c',
  'grd-vnc-pipewire-stream.h',
  'grd-vnc-server.c',
  'grd-vnc-server.h',
])

gen_daemon_sources = []

gen_daemon_sources += gnome.gdbus_codegen('grd-dbus-screen-cast',
                                          'org.gnome.Mutter.ScreenCast.xml',
                                          interface_prefix: 'org.gnome.Mutter.',
                                          namespace: 'GrdDBus')
gen_daemon_sources += gnome.gdbus_codegen('grd-dbus-remote-desktop',
                                          'org.gnome.Mutter.RemoteDesktop.xml',
                                          interface_prefix: 'org.gnome.Mutter.',
                                          namespace: 'GrdDBus')

daemon_sources += gen_daemon_sources

control_sources = ([
  'grd-control.c'
])

prefix = get_option('prefix')
libexecdir = join_paths(prefix, get_option('libexecdir'))

executable('gnome-remote-desktop-daemon',
           daemon_sources,
           dependencies: [glib_dep,
                          gio_dep,
                          gio_unix_dep,
                          pipewire_dep,
                          libvncserver_dep,
                          libsecret_dep,
                          libnotify_dep],
           include_directories: [configinc],
           install: true,
           install_dir: libexecdir)

executable('gnome-remote-desktop-control',
           control_sources,
           dependencies: [glib_dep, gio_dep],
           include_directories: [configinc],
           install : false)

service_config = configuration_data()
service_config.set('libexecdir', libexecdir)

configure_file(input: 'gnome-remote-desktop.service.in',
               output: 'gnome-remote-desktop.service',
               configuration: service_config,
               install_dir: servicedir)

datadir = join_paths(prefix, get_option('datadir'))
schemadir = join_paths(datadir, 'glib-2.0', 'schemas')
custom_target('gsettings-enums',
              input: 'grd-enums.h',
              output: 'org.gnome.desktop.remote-desktop.enums.xml',
              install: true,
              install_dir: join_paths(datadir, 'glib-2.0', 'schemas'),
              capture: true,
              command: ['glib-mkenums',
                '--comments', '<!-- @comment@ -->',
                '--fhead', '<schemalist>',
                '--vhead', '  <@type@ id="org.gnome.desktop.remote-desktop.@EnumName@">',
                '--vprod', '    <value nick="@valuenick@" value="@valuenum@"/>',
                '--vtail', '  </@type@>',
                '--ftail', '</schemalist>',
                '@INPUT@'])
schema = 'org.gnome.desktop.remote-desktop.gschema.xml'
generated_schema = configure_file(output: schema,
                                  input: schema + '.in',
                                  copy: true)
install_data(generated_schema,
             install_dir: schemadir)
gnome.compile_schemas()
