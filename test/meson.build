# Test configuration for grlx-rpc

gtest_dep = dependency('gtest', required: false)

if gtest_dep.found()
  test_sources = [
    'test_rpc.cpp',
  ]

  test_executable = executable(
    'test_rpc',
    sources: test_sources,
    dependencies: [
      grlx_rpc_dep,
      gtest_dep,
      dependency('gtest_main'),
    ],
    include_directories: [
      include_directories('../include'),
    ],
  )

  test('grlx-rpc unit tests', test_executable)
else
  message('Google Test not found, skipping tests')
endif


