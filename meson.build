project('gnome-remote-desktop', 'c',
        version: '40.1',
        meson_version: '>= 0.47.0',
        default_options: ['warning_level=1',
                          'buildtype=debugoptimized'])

freerdp_req = '>= 2.2.0'
fuse_req = '>= 3.9.1'
xkbcommon_req = '>= 1.0.0'

gnome = import('gnome')
i18n  = import('i18n')

cairo_dep = dependency('cairo')
glib_dep = dependency('glib-2.0', version: '>= 2.68')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.3.0')
systemd_dep = dependency('systemd')
libsecret_dep = dependency('libsecret-1')
libnotify_dep = dependency('libnotify')

have_rdp = get_option('rdp')
have_vnc = get_option('vnc')
have_freerdp_2_3 = true

if not have_rdp and not have_vnc
  error('Must enable at least one backend')
endif

if have_rdp
  add_global_arguments('-D_GNU_SOURCE', language : 'c')

  freerdp_dep = dependency('freerdp2', version: '>= 2.3.0', required: false)
  if not freerdp_dep.found()
    freerdp_dep = dependency('freerdp2', version: freerdp_req)
    have_freerdp_2_3 = false
  endif

  freerdp_client_dep = dependency('freerdp-client2', version: freerdp_req)
  freerdp_server_dep = dependency('freerdp-server2', version: freerdp_req)
  fuse_dep = dependency('fuse3', version: fuse_req)
  winpr_dep = dependency('winpr2', version: freerdp_req)
  xkbcommon_dep = dependency('xkbcommon', version: xkbcommon_req)
endif

if have_vnc
  libvncserver_dep = dependency('libvncserver')
  libvncclient_dep = dependency('libvncclient')
endif

cdata = configuration_data()
cdata.set_quoted('GETTEXT_PACKAGE', 'gnome-remote-desktop')
cdata.set_quoted('VERSION', meson.project_version())

cdata.set('HAVE_RDP', have_rdp)
cdata.set('HAVE_VNC', have_vnc)
cdata.set('HAVE_FREERDP_2_3', have_freerdp_2_3)

configure_file(input: 'config.h.meson',
               output: 'config.h',
               configuration: cdata)

configinc = include_directories('.')

servicedir = get_option('systemd_user_unit_dir')
if servicedir == ''
  servicedir = systemd_dep.get_pkgconfig_variable('systemduserunitdir')
endif

if servicedir == ''
  error('Couldn\'t determine systemd user unit service directory')
endif

top_srcdir = meson.current_source_dir()
builddir = meson.current_build_dir()

prefix = get_option('prefix')
libexecdir = join_paths(prefix, get_option('libexecdir'))
datadir = join_paths(prefix, get_option('datadir'))
schemadir = join_paths(datadir, 'glib-2.0', 'schemas')

subdir('src')
subdir('tests')
subdir('po')

meson.add_install_script('meson_post_install.py')

output = [
  '',
  '',
  '   GNOME Remote Desktop ' + meson.project_version(),
  '  ============================',
  '',
  '    Prefix....................... ' + prefix,
  '    libexecdir................... ' + libexecdir,
  '    datadir...................... ' + datadir,
  '    systemd user unit dir........ ' + servicedir,
  '    GSettings schema dir......... ' + schemadir,
  '',
  '    Backends:',
  '',
  '        RDP...................... ' + have_rdp.to_string(),
  '        VNC...................... ' + have_vnc.to_string(),
  '',
  '  Now type \'ninja -C ' + meson.build_root() + '\' to build ' + meson.project_name(),
  '',
  '',
]
message('\n'.join(output))
