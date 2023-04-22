project('gnome-remote-desktop', 'c',
        version: '44.1',
        meson_version: '>= 0.57.0',
        default_options: ['warning_level=1',
                          'buildtype=debugoptimized'])

cuda_req = '>= 11.1.5.0'
epoxy_req = '>= 1.4'
freerdp_req = '>= 2.10.0'
fuse_req = '>= 3.9.1'
xkbcommon_req = '>= 1.0.0'

gnome = import('gnome')
i18n  = import('i18n')

cc = meson.get_compiler('c')

cairo_dep = dependency('cairo')
drm_dep = dependency('libdrm')
epoxy_dep = dependency('epoxy')
glib_dep = dependency('glib-2.0', version: '>= 2.68')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
libnotify_dep = dependency('libnotify')
libsecret_dep = dependency('libsecret-1')
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.3.49')
systemd_dep = dependency('systemd', required: get_option('systemd'))
tss2_esys_dep = dependency('tss2-esys')
tss2_mu_dep = dependency('tss2-mu')
tss2_rc_dep = dependency('tss2-rc')
tss2_tctildr_dep = dependency('tss2-tctildr')

have_fdk_aac = get_option('fdk_aac')
have_rdp = get_option('rdp')
have_vnc = get_option('vnc')

if not have_rdp and not have_vnc
  error('Must enable at least one backend')
endif

if have_fdk_aac and not have_rdp
  error('Support for audio output redirection using FDK AAC requires the RDP backend')
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

  if have_fdk_aac
    fdk_aac_dep = dependency('fdk-aac')
  endif
endif

if have_vnc
  libvncclient_dep = dependency('libvncclient')
  libvncserver_dep = dependency('libvncserver')
endif

prefix = get_option('prefix')
libexecdir = join_paths(prefix, get_option('libexecdir'))
datadir = join_paths(prefix, get_option('datadir'))
mandir = join_paths(prefix, get_option('mandir'))
schemadir = join_paths(datadir, 'glib-2.0', 'schemas')

grd_datadir = join_paths(datadir, 'gnome-remote-desktop')

cdata = configuration_data()
cdata.set_quoted('GETTEXT_PACKAGE', 'gnome-remote-desktop')
cdata.set_quoted('VERSION', meson.project_version())

cdata.set('HAVE_RDP', have_rdp)
cdata.set('HAVE_VNC', have_vnc)
cdata.set('HAVE_FDK_AAC', have_fdk_aac)

cdata.set_quoted('GRD_DATA_DIR', grd_datadir)

configure_file(input: 'config.h.meson',
               output: 'config.h',
               configuration: cdata)

configinc = include_directories('.')

servicedir = get_option('systemd_user_unit_dir')
if systemd_dep.found()
  if servicedir == ''
    servicedir = systemd_dep.get_variable(pkgconfig: 'systemduserunitdir')
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

if get_option('man')
  subdir('man')
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
  '    datadir...................... ' + datadir,
  '    systemd user unit dir........ ' + servicedir,
  '    GSettings schema dir......... ' + schemadir,
  '',
  '    Backends:',
  '',
  '        RDP...................... ' + have_rdp.to_string(),
  '        VNC...................... ' + have_vnc.to_string(),
  '',
  '    Options for the RDP backend:',
  '',
  '        Support for audio output redirection using FDK AAC........' + have_fdk_aac.to_string(),
  '',
  '  Now type \'ninja -C ' + meson.project_build_root() + '\' to build ' + meson.project_name(),
  '',
  '',
]
message('\n'.join(output))
