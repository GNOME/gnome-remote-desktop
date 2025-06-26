sh = find_program('sh', required: true)
prepare_test_environment = find_program('prepare-test-environment.sh', required: true)
dbus_run_session = find_program('dbus-run-session', required: true)

add_test_setup('default',
  is_default: true,
  exe_wrapper: prepare_test_environment,
  env: {
    'TEST_SRCDIR': top_srcdir,
    'TEST_BUILDDIR': builddir,
    'NO_AT_BRIDGE': '1',
    'DBUS_RUN_SESSION_BIN': dbus_run_session.full_path(),
  },
)

assert(have_rdp or have_vnc)


pipewire_test_runner = find_program('pipewire-test-runner.sh', required: true)
pipewire = find_program('pipewire', required: true)
wireplumber = find_program('wireplumber', required: true)
mutter = find_program('mutter', required: true)

integration_tests_env = environment()
integration_tests_env.set('PIPEWIRE_BIN', pipewire.full_path())
integration_tests_env.set('WIREPLUMBER_BIN', wireplumber.full_path())
integration_tests_env.set('MUTTER_BIN', mutter.full_path())
integration_tests_env.set('GSETTINGS_SCHEMA_DIR', fs.parent(generated_schema.full_path()))

if have_rdp
  test_rsa_data = custom_target('test-rsa-data',
    output: [
      'tls.crt',
      'tls.key',
    ],
    command: [
      find_program('openssl'),
      'req', '-new', '-newkey', 'rsa:4096',
      '-days', '720', '-nodes', '-x509',
      '-subj', '/C=DE/ST=NONE/L=NONE/O=GNOME/CN=gnome.org',
      '-out', '@OUTPUT0@',
      '-keyout', '@OUTPUT1@',
    ],
  )

  test_client_rdp = executable(
    'test-client-rdp',
    files(['test-client-rdp.c']),
    dependencies: [glib_dep,
                   freerdp_dep,
                   freerdp_client_dep,
                   winpr_dep],
    include_directories: [configinc],
    install: false)

  test_env = integration_tests_env
  test_env.set('WLOG_LEVEL', 'DEBUG')
  test_env.set('G_MESSAGES_DEBUG', 'all')
  test_file = find_program('run-rdp-tests.py')

  test('gnome-remote-desktop/rdp', sh,
    args: [
      pipewire_test_runner.full_path(),
      '--enable-rdp',
      '--rdp-tls-cert', test_rsa_data[0].full_path(),
      '--rdp-tls-key', test_rsa_data[1].full_path(),
      '--',
      dbus_run_session.full_path(),
      '--',
      test_file.full_path(),
    ],
    env: test_env,
    depends: [
      test_rsa_data,
    ],
    is_parallel: false,
    timeout: 20,
  )
endif

if have_vnc
  test_client_vnc = executable(
    'test-client-vnc',
    files(['test-client-vnc.c']),
    dependencies: [glib_dep,
    libvncclient_dep],
    include_directories: [configinc],
    install: false)

  test_file = find_program('run-vnc-tests.py')

  test('gnome-remote-desktop/vnc', sh,
    args: [
      pipewire_test_runner.full_path(),
      '--enable-vnc',
      '--',
      dbus_run_session.full_path(),
      '--',
      test_file.full_path(),
    ],
    env: integration_tests_env,
    is_parallel: false,
    timeout: 20,
  )
endif

gbm_dep = dependency('gbm')
gudev_dep = dependency('gudev-1.0')

egl_thread_test = executable(
  'egl-thread-test',
  sources: [
    'egl-thread-test.c',
    '../src/grd-egl-thread.c',
    '../src/grd-egl-thread.h',
  ],
  dependencies: [
    deps,
    drm_dep,
    gbm_dep,
    gudev_dep,
  ],
  include_directories: [
    src_includepath,
    configinc,
  ],
)

tpm_test = executable(
  'tpm-test',
  sources: [
    'tpm-test.c',
    '../src/grd-debug.c',
    '../src/grd-debug.h',
    '../src/grd-tpm.c',
    '../src/grd-tpm.h',
  ],
  dependencies: [
    deps,
  ],
  include_directories: [
    src_includepath,
    configinc,
  ],
)

test('egl-thread', egl_thread_test)
test('tpm', tpm_test)
