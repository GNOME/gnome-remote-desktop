if have_rdp
  install_data(['grd-cuda-damage-utils_30.ptx'],
    install_dir: grd_datadir,
  )
  install_data(['grd-cuda-avc-utils_30.ptx'],
    install_dir: grd_datadir,
  )

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

    install_data('org.gnome.RemoteDesktop.Handover.target',
      install_dir: userunitdir,
    )

    install_symlink('org.gnome.RemoteDesktop.Handover.target',
      pointing_to: '../org.gnome.RemoteDesktop.Handover.target',
      install_dir: join_paths(userunitdir, 'gnome-session.target.wants')
    )
  endif
endif

configure_file(input: 'org.gnome.remotedesktop.configure-system-daemon.policy.in',
               output: 'org.gnome.remotedesktop.configure-system-daemon.policy',
               configuration: {
                 'bindir': bindir,
               },
               install_dir: join_paths(datadir, 'polkit-1', 'actions'))
