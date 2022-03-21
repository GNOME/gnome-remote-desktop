project('gnome-remote-desktop', 'c',
        version: '42.0',
        meson_version: '>= 0.47.0',
        default_options: ['warning_level=1',
                          'buildtype=debugoptimized'])

cuda_req = '>= 11.1.5.0'
freerdp_req = '>= 2.5.0'
fuse_req = '>= 3.9.1'
xkbcommon_req = '>= 1.0.0'
epoxy_req = '>= 1.4'

gnome = import('gnome')
i18n  = import('i18n')

cc = meson.get_compiler('c')

cairo_dep = dependency('cairo')
glib_dep = dependency('glib-2.0', version: '>= 2.68')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.3.0')
systemd_dep = dependency('systemd', required: get_option('systemd'))
libsecret_dep = dependency('libsecret-1')
libnotify_dep = dependency('libnotify')
epoxy_dep = dependency('epoxy')
drm_dep = dependency('libdrm')

have_rdp = get_option('rdp')
have_vnc = get_option('vnc')

if not have_rdp and not have_vnc
  error('Must enable at least one backend')
endif

if have_rdp
  add_global_arguments('-D_GNU_SOURCE', language : 'c')

  dl_dep = cc.find_library('dl', required: true)

  cuda_dep = dependency('ffnvcodec', version: cuda_req)
  freerdp_dep = dependency('freerdp2', version: freerdp_req)
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

prefix = get_option('prefix')
libexecdir = join_paths(prefix, get_option('libexecdir'))
datadir = join_paths(prefix, get_option('datadir'))
schemadir = join_paths(datadir, 'glib-2.0', 'schemas')

grd_datadir = join_paths(datadir, 'gnome-remote-desktop')

cdata = configuration_data()
cdata.set_quoted('GETTEXT_PACKAGE', 'gnome-remote-desktop')
cdata.set_quoted('VERSION', meson.project_version())

cdata.set('HAVE_RDP', have_rdp)
cdata.set('HAVE_VNC', have_vnc)

cdata.set_quoted('GRD_DATA_DIR', grd_datadir)

configure_file(input: 'config.h.meson',
               output: 'config.h',
               configuration: cdata)

configinc = include_directories('.')

servicedir = get_option('systemd_user_unit_dir')
if systemd_dep.found()
  if servicedir == ''
    servicedir = systemd_dep.get_pkgconfig_variable('systemduserunitdir')
  endif

  if servicedir == ''
    error('Couldn\'t determine systemd user unit service directory')
  endif
endif

top_srcdir = meson.current_source_dir()
builddir = meson.current_build_dir()

subdir('data')
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
