demo_exe = executable(
  'llmghost-demo',
  'demo.c',
  dependencies: [llmghost_dep],
  install: false,
)

test_fim_tokens = executable(
  'test-fim-tokens',
  'test-fim-tokens.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('fim-tokens', test_fim_tokens, suite: 'unit')

test_ollama_body = executable(
  'test-ollama-body',
  'test-ollama-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('ollama-body', test_ollama_body, suite: 'unit')

test_fake_backend = executable(
  'test-fake-backend',
  'test-fake-backend.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('fake-backend', test_fake_backend, suite: 'unit')

mock_backend_sources = files('mock-backend.c')

test_mock_backend = executable(
  'test-mock-backend',
  ['test-mock-backend.c', mock_backend_sources],
  dependencies: [llmghost_dep],
  install: false,
)
test('mock-backend', test_mock_backend, suite: 'unit')

if xvfb_run.found()
  test_controller = executable(
    'test-controller',
    ['test-controller.c', mock_backend_sources],
    dependencies: [llmghost_dep],
    install: false,
  )
  # Wrap just this test in xvfb-run, so the unit suite needs no display.
  test('controller', xvfb_run, args: ['-a', test_controller], suite: 'gui')
endif
