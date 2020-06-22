project('gnome-remote-desktop', 'c',
        version: '0.1.8',
        meson_version: '>= 0.47.0',
        default_options: ['warning_level=1',
                          'buildtype=debugoptimized'])

gnome = import('gnome')

cairo_dep = dependency('cairo')
glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
pipewire_dep = dependency('libpipewire-0.3', version: '>= 0.3.0')
systemd_dep = dependency('systemd')
libvncserver_dep = dependency('libvncserver')
libvncclient_dep = dependency('libvncclient')
libsecret_dep = dependency('libsecret-1')
libnotify_dep = dependency('libnotify')

cdata = configuration_data()
cdata.set_quoted('GETTEXT_PACKAGE', 'gnome-remote-desktop')
cdata.set_quoted('VERSION', meson.project_version())

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

subdir('src')
subdir('tests')

meson.add_install_script('meson_post_install.py')
