#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_FAKE_BACKEND (llm_ghost_fake_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostFakeBackend, llm_ghost_fake_backend,
                      LLM_GHOST, FAKE_BACKEND, GObject)

LlmGhostBackend *llm_ghost_fake_backend_new (const char *canned_response);

G_END_DECLS
