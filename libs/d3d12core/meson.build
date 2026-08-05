d3d12core_src = [
  'debug.c',
  'main.c'
]

if vkd3d_platform == 'windows'
  d3d12core_name_prefix = ''
  d3d12core_dependencies = [ vkd3d_dep, lib_gdi32, lib_dxgi ]
else
  d3d12core_name_prefix = 'libvkd3d-proton-'
  d3d12core_dependencies = [ vkd3d_dep, lib_dl ]
endif

d3d12core_needs_defs = (not vkd3d_is_msvc) and (vkd3d_platform == 'windows')

d3d12core_lib = shared_library(d3d12core_name_prefix + 'd3d12core', d3d12core_src,
  name_prefix         : '',
  dependencies        : d3d12core_dependencies,
  include_directories : vkd3d_private_includes,
  install             : true,
  objects             : d3d12core_needs_defs ? 'd3d12core.def' : [],
  vs_module_defs      : 'd3d12core.def')

# Helios addition: the same sources and dependencies as d3d12core, plus
# helios_entry.c, emitted under a name that cannot shadow the OS d3d12core.dll in
# a LoadLibrary search.  A second target rather than a rename of d3d12core_lib, so
# that rebasing onto upstream does not conflict in this file.
# It needs its OWN .def: d3d12core.def opens with `LIBRARY d3d12core.dll`, which
# names the output, and reusing it here would stamp the import library and export
# table with the wrong module name.
helios_vkd3d_lib = shared_library('helios_vkd3d', d3d12core_src + [ 'helios_entry.c' ],
  name_prefix         : '',
  dependencies        : d3d12core_dependencies,
  include_directories : vkd3d_private_includes,
  install             : true,
  objects             : d3d12core_needs_defs ? 'helios_vkd3d.def' : [],
  vs_module_defs      : 'helios_vkd3d.def')

d3d12core_dep = declare_dependency(
  link_with           : d3d12core_lib,
  include_directories : vkd3d_public_includes)
