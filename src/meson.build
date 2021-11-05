src_includepath = include_directories('.')

deps = [
  cairo_dep,
  glib_dep,
  gio_dep,
  gio_unix_dep,
  pipewire_dep,
  libsecret_dep,
  libnotify_dep,
  epoxy_dep,
  drm_dep,
]

daemon_sources = files([
  'grd-clipboard.c',
  'grd-clipboard.h',
  'grd-context.c',
  'grd-context.h',
  'grd-egl-thread.c',
  'grd-egl-thread.h',
  'grd-daemon.c',
  'grd-daemon.h',
  'grd-damage-utils.c',
  'grd-damage-utils.h',
  'grd-mime-type.c',
  'grd-mime-type.h',
  'grd-pipewire-utils.c',
  'grd-pipewire-utils.h',
  'grd-private.h',
  'grd-prompt.c',
  'grd-prompt.h',
  'grd-session.c',
  'grd-session.h',
  'grd-settings.c',
  'grd-settings.h',
  'grd-stream.c',
  'grd-stream.h',
  'grd-types.h',
])

if have_rdp
  daemon_sources += files([
    'grd-clipboard-rdp.c',
    'grd-clipboard-rdp.h',
    'grd-rdp-event-queue.c',
    'grd-rdp-event-queue.h',
    'grd-rdp-frame-info.h',
    'grd-rdp-fuse-clipboard.c',
    'grd-rdp-fuse-clipboard.h',
    'grd-rdp-gfx-frame-log.c',
    'grd-rdp-gfx-frame-log.h',
    'grd-rdp-gfx-surface.c',
    'grd-rdp-gfx-surface.h',
    'grd-rdp-graphics-pipeline.c',
    'grd-rdp-graphics-pipeline.h',
    'grd-rdp-network-autodetection.c',
    'grd-rdp-network-autodetection.h',
    'grd-rdp-pipewire-stream.c',
    'grd-rdp-pipewire-stream.h',
    'grd-rdp-private.h',
    'grd-rdp-sam.c',
    'grd-rdp-sam.h',
    'grd-rdp-server.c',
    'grd-rdp-server.h',
    'grd-rdp-surface.c',
    'grd-rdp-surface.h',
    'grd-session-rdp.c',
    'grd-session-rdp.h',
  ])

  deps += [
    freerdp_dep,
    freerdp_client_dep,
    freerdp_server_dep,
    fuse_dep,
    winpr_dep,
    xkbcommon_dep,
  ]

  if have_nvenc
    daemon_sources += files([
      'grd-rdp-nvenc.c',
      'grd-rdp-nvenc.h',
    ])

    deps += [
      dl_dep,
      nvenc_dep,
    ]
  endif
endif

if have_vnc
  daemon_sources += files([
    'grd-clipboard-vnc.c',
    'grd-clipboard-vnc.h',
    'grd-session-vnc.c',
    'grd-session-vnc.h',
    'grd-vnc-cursor.c',
    'grd-vnc-cursor.h',
    'grd-vnc-pipewire-stream.c',
    'grd-vnc-pipewire-stream.h',
    'grd-vnc-server.c',
    'grd-vnc-server.h',
  ])

  deps += [
    libvncserver_dep,
  ]
endif

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

executable('gnome-remote-desktop-daemon',
           daemon_sources,
           dependencies: deps,
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
