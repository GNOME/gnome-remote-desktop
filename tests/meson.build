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
