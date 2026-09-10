
vkd3d_test_flags = []
vkd3d_test_deps = [ lib_d3d12, vkd3d_common_dep ]
if vkd3d_platform != 'windows'
  executable('helios-native-admission', 'helios_native_admission.c',
    dependencies: [vkd3d_dep, lib_dl],
    link_with: [helios_d3d12_static_lib],
    include_directories: [vkd3d_private_includes, include_directories('../libs/vkd3d', '..')],
    install: false)
  executable('helios-root-contract', 'helios_root_contract.c',
    dependencies: [vkd3d_dep, lib_dl],
    link_with: [helios_d3d12_static_lib],
    include_directories: [vkd3d_private_includes, include_directories('../libs/vkd3d', '..')],
    install: false)
endif
if vkd3d_platform == 'windows'
  vkd3d_test_deps += [ lib_dxgi ]
endif

d3d12_test_utils_lib = static_library('d3d12-test-utils', 'd3d12_test_utils.c',
  dependencies        : vkd3d_test_deps,
  include_directories : vkd3d_private_includes)

if vkd3d_platform == 'windows'
  executable('helios-native-features',
    ['helios_native_features.c', 'helios_native_debug.cpp', 'd3d12_rov.c', 'd3d12_command.c', 'd3d12_tir.c', 'd3d12_query.c',
      'd3d12_device.c', 'd3d12_shaders.c', 'd3d12_geometry_shader.c', 'd3d12_tessellation.c', 'd3d12_descriptors.c'],
    dependencies: vkd3d_test_deps + [vkd3d_compiler.find_library('advapi32')],
    include_directories: vkd3d_private_includes,
    link_with: [d3d12_test_utils_lib],
    install: false)
endif

d3d12_test_src = [
  'd3d12_tir.c',
  'd3d12_dxvk_interop_device.c',
  'd3d12_clip_cull_distance.c',
  'd3d12_enhanced_barriers.c',
  'd3d12_sampler_feedback.c',
  'd3d12_geometry_shader.c',
  'd3d12_win32_exclusive.c',
  'd3d12_root_signature.c',
  'd3d12_depth_stencil.c',
  'd3d12_render_target.c',
  'd3d12_tessellation.c',
  'd3d12_sm_advanced.c',
  'd3d12_mesh_shader.c',
  'd3d12_descriptors.c',
  'd3d12_raytracing.c',
  'd3d12_robustness.c',
  'd3d12_workgraphs.c',
  'd3d12_streamout.c',
  'd3d12_pso_blob.c',
  'd3d12_resource.c',
  'd3d12_bindless.c',
  'd3d12_dstorage.c',
  'd3d12_shaders.c',
  'd3d12_command.c',
  'd3d12_sparse.c',
  'd3d12_device.c',
  'd3d12_clear.c',
  'd3d12_query.c',
  'd3d12_sync.c',
  'd3d12_copy.c',
  'd3d12_vrs.c',
  'd3d12_pso.c',
  'd3d12_rov.c',
  'd3d12_va.c',
  'd3d12.c',
]

if vkd3d_platform == 'windows'
  d3d12_test_src += ['helios_native_debug.cpp']
endif

executable('d3d12', d3d12_test_src,
  dependencies        : vkd3d_test_deps,
  include_directories : vkd3d_private_includes,
  install             : false,
  c_args              : vkd3d_test_flags,
  link_with           : [ d3d12_test_utils_lib ])

executable('descriptor-performance', 'descriptor_performance.c',
  dependencies        : vkd3d_test_deps,
  include_directories : vkd3d_private_includes,
  install             : false,
  c_args              : vkd3d_test_flags,
  link_with           : [ d3d12_test_utils_lib ])

executable('pso-library-bloat', 'pso_library_bloat.c',
  dependencies        : vkd3d_test_deps,
  include_directories : vkd3d_private_includes,
  install             : false,
  c_args              : vkd3d_test_flags,
  link_with           : [ d3d12_test_utils_lib ])

if host_machine.system() == 'linux' and vkd3d_compiler.has_function('__libc_calloc',
    prefix: '#include <stddef.h>\nextern void *__libc_calloc(size_t, size_t);')
  shared_library('rtas-prebuild-alloc-fault', 'rtas_prebuild_alloc_fault.c',
    install: false)
endif
