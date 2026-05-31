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
