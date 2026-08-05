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

# Helios addition: the RETIRED dynamic arm.  ⛔ Not the shipping path — see
# helios_d3d12_static_lib below, which is.  Retained only so D12-G1 (the engine
# gate, passed 2026-08-05 against this target) stays reproducible; delete it once
# G1 has been re-run against the static arm.
#
# The same sources and dependencies as d3d12core, plus
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

# Helios addition — THE SHIPPING ARM: static.
#
# Owner decision 2026-08-05 (DECISIONS.md D4): "we are going to statically link
# vkd3d-proton and not mess with dynamic dlls."  helios_umd12.dll links these
# archives directly, exactly as helios_umd.dll links DXVK's.
#
# Same Helios entry points as helios_vkd3d_lib above, but emitted as a STATIC
# library and, critically, WITHOUT main.c and WITHOUT lib_dxgi:
#
#   * main.c is the ONLY object in the entire engine that references
#     CreateDXGIFactory1.  Measured with `nm --undefined-only` over
#     libvkd3d-proton.a, libvkd3d-shader.a, libvkd3d_common.a, libdxil-spirv.a and
#     libdxbc_spv.a: zero references in all five.  A shared_library force-links
#     every object handed to it, which is why helios_vkd3d.dll carries a static
#     `dxgi.dll` import for a function it never calls.  An archive member is
#     pulled only when referenced, so leaving main.c out here means the import
#     cannot be generated at all.  That is what D4 reason 1 always wanted and what
#     the DLL arm never actually delivered.
#   * With main.c gone nothing references lib_dxgi, so it is dropped too.
#
# debug.c IS kept: helios_entry.c uses TRACE/ERR/debugstr_guid, and it carries no
# dxgi reference of its own (verified).
#
# No .def and no vs_module_defs, deliberately — a static library has no export
# table.  helios_umd12.dll does not re-export these, it calls them.
helios_d3d12_static_lib = static_library('helios_d3d12_static',
  [ 'debug.c', 'helios_entry.c' ],
  dependencies        : [ vkd3d_dep ],
  include_directories : vkd3d_private_includes,
  install             : true)

d3d12core_dep = declare_dependency(
  link_with           : d3d12core_lib,
  include_directories : vkd3d_public_includes)
