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

test_ghost_accept = executable(
  'test-ghost-accept',
  'test-ghost-accept.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('ghost-accept', test_ghost_accept, suite: 'unit')

test_http_util = executable(
  'test-http-util',
  'test-http-util.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('http-util', test_http_util, suite: 'unit')

test_openai_body = executable(
  'test-openai-body',
  'test-openai-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('openai-body', test_openai_body, suite: 'unit')

test_mistral_body = executable(
  'test-mistral-body',
  'test-mistral-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('mistral-body', test_mistral_body, suite: 'unit')

test_settings = executable(
  'test-settings',
  'test-settings.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('settings', test_settings, suite: 'unit')

test_text_util = executable(
  'test-text-util',
  'test-text-util.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('text-util', test_text_util, suite: 'unit')

test_generic_body = executable(
  'test-generic-body',
  'test-generic-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('generic-body', test_generic_body, suite: 'unit')

test_secret_store = executable(
  'test-secret-store',
  'test-secret-store.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('secret-store', test_secret_store, suite: 'unit')

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
