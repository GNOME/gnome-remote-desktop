if have_rdp
  test_client_rdp = executable(
    'test-client-rdp',
    files(['test-client-rdp.c']),
    dependencies: [glib_dep,
                   freerdp_dep,
                   freerdp_client_dep,
                   winpr_dep],
    include_directories: [configinc],
    install: false)

  test_runner = find_program('rdp-test-runner.sh')

  test_env = environment()
  test_env.set('TEST_SRCDIR', top_srcdir)
  test_env.set('TEST_BUILDDIR', builddir)
  test_env.set('NO_AT_BRIDGE', '1')

  test('gnome-remote-desktop/rdp', test_runner,
    env: test_env,
    is_parallel: false,
    timeout: 10,
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

  test_runner = find_program('vnc-test-runner.sh')

  test_env = environment()
  test_env.set('TEST_SRCDIR', top_srcdir)
  test_env.set('TEST_BUILDDIR', builddir)
  test_env.set('NO_AT_BRIDGE', '1')

  test('gnome-remote-desktop/vnc', test_runner,
    env: test_env,
    is_parallel: false,
    timeout: 10,
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
