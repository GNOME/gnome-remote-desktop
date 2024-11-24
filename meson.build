project('gnome-remote-desktop', 'c',
        version: '47.2',
        meson_version: '>= 0.58.0',
        default_options: ['warning_level=1',
                          'buildtype=debugoptimized'])

cuda_req = '>= 11.1.5.0'
epoxy_req = '>= 1.4'
freerdp_req = '>= 3.1.0'
fuse_req = '>= 3.9.1'
polkit_req = '>= 122'
xkbcommon_req = '>= 1.0.0'
libei_req = '>= 1.2.0'

gnome = import('gnome')
i18n  = import('i18n')
fs  = import('fs')

cc = meson.get_compiler('c')

cairo_dep = dependency('cairo')
drm_dep = dependency('libdrm')
epoxy_dep = dependency('epoxy')
glib_dep = dependency('glib-2.0', version: '>= 2.75.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
libei_dep = dependency('libei-1.0', version: libei_req)
libnotify_dep = dependency('libnotify')
libsecret_dep = dependency('libsecret-1')
libsystemd_dep = dependency('libsystemd', required: false)
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.3.49')
systemd_dep = dependency('systemd', required: get_option('systemd'))
tss2_esys_dep = dependency('tss2-esys')
tss2_mu_dep = dependency('tss2-mu')
tss2_rc_dep = dependency('tss2-rc')
tss2_tctildr_dep = dependency('tss2-tctildr')
xkbcommon_dep = dependency('xkbcommon', version: xkbcommon_req)

have_rdp = get_option('rdp')
have_vnc = get_option('vnc')

if not have_rdp and not have_vnc
  error('Must enable at least one backend')
endif

if have_rdp
  add_global_arguments('-D_GNU_SOURCE', language : 'c')

  dl_dep = cc.find_library('dl', required: true)

  cuda_dep = dependency('ffnvcodec', version: cuda_req)
  fdk_aac_dep = dependency('fdk-aac')
  freerdp_dep = dependency('freerdp3', version: freerdp_req)
  freerdp_client_dep = dependency('freerdp-client3', version: freerdp_req)
  freerdp_server_dep = dependency('freerdp-server3', version: freerdp_req)
  fuse_dep = dependency('fuse3', version: fuse_req)
  opus_dep = dependency('opus')
  polkit_dep = dependency('polkit-gobject-1', version: polkit_req)
  winpr_dep = dependency('winpr3', version: freerdp_req)
endif

if have_vnc
  libvncclient_dep = dependency('libvncclient')
  libvncserver_dep = dependency('libvncserver')
endif

prefix = get_option('prefix')
libexecdir = join_paths(prefix, get_option('libexecdir'))
bindir = join_paths(prefix, get_option('bindir'))
datadir = join_paths(prefix, get_option('datadir'))
mandir = join_paths(prefix, get_option('mandir'))
schemadir = join_paths(datadir, 'glib-2.0', 'schemas')
desktopdir = join_paths(datadir, 'applications')
dbus_sys_dir = (get_option('dbus_sys_dir') != '') ? get_option('dbus_sys_dir') : join_paths(datadir, 'dbus-1', 'system.d')

dbus_dep = dependency('dbus-1', required: false)
dbus_system_services_dir = dbus_dep.get_variable(datadir / 'dbus-1' / 'system-services',
                                                 pkgconfig: 'system_bus_services_dir',
                                                 pkgconfig_define: ['datadir', datadir])

grd_datadir = join_paths(datadir, 'gnome-remote-desktop')

grd_confdir = get_option('conf_dir')
if grd_confdir == ''
  grd_confdir = join_paths(get_option('sysconfdir'), 'gnome-remote-desktop')
endif
grd_custom_conf = join_paths(grd_confdir, 'grd.conf')
grd_default_conf = join_paths(grd_datadir, 'grd.conf')

grd_username = get_option('user')
grd_homedir = get_option('home_dir')

cdata = configuration_data()
cdata.set_quoted('GETTEXT_PACKAGE', 'gnome-remote-desktop')
cdata.set_quoted('VERSION', meson.project_version())

cdata.set('HAVE_LIBSYSTEMD', libsystemd_dep.found())
cdata.set('HAVE_RDP', have_rdp)
cdata.set('HAVE_VNC', have_vnc)

cdata.set_quoted('GRD_DATA_DIR', grd_datadir)
cdata.set_quoted('GRD_LIBEXEC_DIR', libexecdir)
cdata.set_quoted('GRD_CUSTOM_CONF', grd_custom_conf)
cdata.set_quoted('GRD_DEFAULT_CONF', grd_default_conf)
cdata.set_quoted('GRD_USERNAME', grd_username)

configure_file(input: 'config.h.meson',
               output: 'config.h',
               configuration: cdata)

configinc = include_directories('.')

userunitdir = get_option('systemd_user_unit_dir')
systemunitdir = get_option('systemd_system_unit_dir')
sysusersdir = get_option('systemd_sysusers_dir')
tmpfilesdir = get_option('systemd_tmpfiles_dir')
if systemd_dep.found()
  if userunitdir == ''
    userunitdir = systemd_dep.get_variable(pkgconfig: 'systemduserunitdir')
  endif

  if userunitdir == ''
    error('Couldn\'t determine systemd user unit service directory')
  endif

  if systemunitdir == ''
    systemunitdir = systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir')
  endif

  if systemunitdir == ''
    error('Couldn\'t determine systemd system unit service directory')
  endif

  if sysusersdir == ''
    sysusersdir = systemd_dep.get_variable(pkgconfig: 'sysusersdir')
  endif

  if sysusersdir == ''
    error('Couldn\'t determine systemd sysusers directory')
  endif

  if tmpfilesdir == ''
    tmpfilesdir = systemd_dep.get_variable(pkgconfig: 'tmpfilesdir')
  endif

  if tmpfilesdir == ''
    error('Couldn\'t determine systemd tmpfilesdir directory')
  endif
endif

top_srcdir = meson.current_source_dir()
builddir = meson.current_build_dir()

subdir('data')
subdir('src')
subdir('po')

if get_option('man')
  subdir('man')
endif

if get_option('tests')
  subdir('tests')
endif

gnome.post_install(
  glib_compile_schemas: true,
)

output = [
  '',
  '',
  '   GNOME Remote Desktop ' + meson.project_version(),
  '  ============================',
  '',
  '    Prefix....................... ' + prefix,
  '    libexecdir................... ' + libexecdir,
  '    bindir....................... ' + bindir,
  '    datadir...................... ' + datadir,
  '    desktopdir................... ' + desktopdir,
  '    systemd user unit dir........ ' + userunitdir,
  '    systemd system unit dir...... ' + systemunitdir,
  '    GSettings schema dir......... ' + schemadir,
  '    System DBus dir.............. ' + dbus_sys_dir,
  '',
  '    Backends:',
  '',
  '        RDP...................... ' + have_rdp.to_string(),
  '        VNC...................... ' + have_vnc.to_string(),
  '',
  '  Now type \'ninja -C ' + meson.project_build_root() + '\' to build ' + meson.project_name(),
  '',
  '',
]
message('\n'.join(output))
