project(
  'grlx-rpc',
  'cpp',
  version: '1.0.0',
  license: 'MIT',
  default_options: ['cpp_std=c++20'],
)

# grlx-rpc is a header-only library
cpp = meson.get_compiler('cpp')
system = host_machine.system()

# Dependencies
grlx_rpc_deps = []
grlx_rpc_build_args = []

if system == 'emscripten'
  # Emscripten-specific settings
  grlx_rpc_build_args += '-fbracket-depth=512'
  grlx_rpc_deps += [
    dependency('boost', modules: ['system']),
  ]
else
  # Native platform dependencies
  grlx_rpc_deps += [
    dependency('boost', modules: ['system']),
    dependency('threads'),
    dependency('spdlog'),
  ]
endif

# Public headers
grlx_rpc_public_headers = include_directories('include')

# Header-only library - no sources to compile
grlx_rpc_target = static_library(
  'grlx-rpc',
  sources: [],  # Header-only
  dependencies: grlx_rpc_deps,
  include_directories: [
    grlx_rpc_public_headers,
    include_directories('include/grlx'),
    include_directories('include/grlx/rpc')
  ],
  cpp_args: grlx_rpc_build_args,
)

# Make this library usable as a Meson subproject
grlx_rpc_dep = declare_dependency(
  include_directories: [grlx_rpc_public_headers],
  dependencies: grlx_rpc_deps,
  link_with: grlx_rpc_target,
)

# Export the dependency for parent projects
set_variable('grlx_rpc_dep', grlx_rpc_dep)

# ==========
# Unit Tests
# ==========

if not meson.is_subproject() and system != 'emscripten'
  subdir('test')
endif

# Optionally install headers
# install_headers(
#   ... list of headers ...
#   subdir: 'grlx-rpc'
# )

