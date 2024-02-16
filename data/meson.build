if have_rdp
  install_data(['grd-cuda-damage-utils_30.ptx'],
    install_dir: grd_datadir,
  )
  install_data(['grd-cuda-avc-utils_30.ptx'],
    install_dir: grd_datadir,
  )

  configure_file(input: 'grd.conf.in',
                 output: 'grd.conf',
                 configuration: {
                   'GRD_CUSTOM_CONF': grd_custom_conf,
                   'GRD_CONFDIR': grd_confdir,
                 },
                 install_dir: grd_datadir)

  configure_file(input: 'gnome-remote-desktop-sysusers.conf.in',
                 output: 'gnome-remote-desktop-sysusers.conf',
                 configuration: {
                   'GRD_USERNAME': grd_username,
                   'GRD_HOMEDIR': grd_homedir,
                 },
                 install_dir: sysusersdir)

  configure_file(input: 'gnome-remote-desktop-tmpfiles.conf.in',
                 output: 'gnome-remote-desktop-tmpfiles.conf',
                 configuration: {
                   'GRD_USERNAME': grd_username,
                   'GRD_HOMEDIR': grd_homedir,
                   'GRD_CONFDIR': grd_confdir,
                 },
                 install_dir: tmpfilesdir)

  configure_file(input: 'org.gnome.RemoteDesktop.conf.in',
                 output: 'org.gnome.RemoteDesktop.conf',
                 configuration: {
                   'GRD_USERNAME': grd_username,
                 },
                 install_dir: dbus_sys_dir)

  configure_file(input: 'org.gnome.RemoteDesktop.Handover.desktop.in',
                 output: 'org.gnome.RemoteDesktop.Handover.desktop',
                 configuration: {
                   'libexecdir': libexecdir,
                 },
                 install_dir: desktopdir)
endif

if get_option('systemd')
  configure_file(input: 'gnome-remote-desktop.service.in',
                 output: 'gnome-remote-desktop.service',
                 configuration: {
                   'libexecdir': libexecdir,
                 },
                 install_dir: userunitdir)
  configure_file(input: 'gnome-remote-desktop-headless.service.in',
                 output: 'gnome-remote-desktop-headless.service',
                 configuration: {
                   'libexecdir': libexecdir,
                 },
                 install_dir: userunitdir)

  if have_rdp
    grd_system_service = configure_file(input: 'gnome-remote-desktop-system.service.in',
                                        output: 'gnome-remote-desktop-system.service',
                                        configuration: {
                                          'libexecdir': libexecdir,
                                          'GRD_USERNAME': grd_username,
                                        })
    install_data(grd_system_service,
      rename: 'gnome-remote-desktop.service',
      install_dir: systemunitdir
    )

    configure_file(input: 'gnome-remote-desktop-handover.service.in',
                   output: 'gnome-remote-desktop-handover.service',
                   configuration: {
                     'libexecdir': libexecdir,
                   },
                   install_dir: userunitdir)
  endif
endif

configure_file(input: 'org.gnome.remotedesktop.configure-system-daemon.policy.in',
               output: 'org.gnome.remotedesktop.configure-system-daemon.policy',
               configuration: {
                 'bindir': bindir,
                 'GRD_USERNAME': grd_username,
               },
               install_dir: join_paths(datadir, 'polkit-1', 'actions'))

configure_file(input: 'org.gnome.RemoteDesktop.service.in',
               output: 'org.gnome.RemoteDesktop.service',
               configuration: {
                 'libexecdir': libexecdir,
                 'GRD_USERNAME': grd_username,
               },
               install_dir: dbus_system_services_dir)
