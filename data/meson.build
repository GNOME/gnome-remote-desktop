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
endif

configure_file(input: 'org.gnome.remotedesktop.configure-system-daemon.policy.in',
               output: 'org.gnome.remotedesktop.configure-system-daemon.policy',
               configuration: {
                 'bindir': bindir,
               },
               install_dir: join_paths(datadir, 'polkit-1', 'actions'))
