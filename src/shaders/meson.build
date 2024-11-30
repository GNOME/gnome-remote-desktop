if have_rdp
  shaders = [
    'grd-avc-stereo-view',
  ]

  spirv_sources = []

  foreach shader : shaders
    run_command(glslc, '-o', shader + '.spv', shader + '.comp', check: true)
    run_command(spirv_opt, '-o', shader + '_opt.spv', '-O', shader + '.spv', check: true)
    spirv_sources += [shader + '_opt.spv']
  endforeach

  install_data(spirv_sources,
    install_dir: grd_shaderdir
  )
endif
