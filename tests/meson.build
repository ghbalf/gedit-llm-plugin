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
