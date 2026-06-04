#pragma once

#include "llmghost-backend.h"
#include "llmghost-settings.h"

G_BEGIN_DECLS

/**
 * llm_ghost_backend_new_from_settings:
 * @settings: loaded settings.
 *
 * Builds the backend named by the active "backend" key, configured from its
 * "backends.<name>" params. An unknown name falls back to a default Ollama
 * backend. Returns a new reference the caller owns.
 */
LlmGhostBackend *llm_ghost_backend_new_from_settings (LlmGhostSettings *settings);

G_END_DECLS
