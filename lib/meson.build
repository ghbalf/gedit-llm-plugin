llmghost_sources = files(
  'llmghost-backend.c',
  'llmghost-backend-factory.c',
  'llmghost-controller.c',
  'llmghost-fake-backend.c',
  'llmghost-fim-tokens.c',
  'llmghost-http-util.c',
  'llmghost-ollama-backend.c',
  'llmghost-mistral-backend.c',
  'llmghost-openai-backend.c',
  'llmghost-overlay.c',
  'llmghost-settings.c',
)

llmghost_headers = files(
  'llmghost.h',
  'llmghost-backend.h',
  'llmghost-backend-factory.h',
  'llmghost-controller.h',
  'llmghost-fake-backend.h',
  'llmghost-fim-tokens.h',
  'llmghost-ollama-backend.h',
  'llmghost-mistral-backend.h',
  'llmghost-openai-backend.h',
  'llmghost-overlay.h',
  'llmghost-settings.h',
)

llmghost_deps = [
  gtk_dep, glib_dep, gobject_dep, gio_dep, soup_dep, json_dep,
]

llmghost_lib = static_library(
  'llmghost',
  llmghost_sources,
  dependencies: llmghost_deps,
  include_directories: llmghost_inc,
  install: false,
)

llmghost_dep = declare_dependency(
  link_with: llmghost_lib,
  include_directories: llmghost_inc,
  dependencies: llmghost_deps,
)
