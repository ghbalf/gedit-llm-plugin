#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

typedef enum
{
  LLM_GHOST_OPENAI_MODE_COMPLETIONS,   /* /v1/completions, native FIM via suffix */
  LLM_GHOST_OPENAI_MODE_CHAT,          /* /v1/chat/completions, prompt-engineered FIM */
} LlmGhostOpenAIMode;

#define LLM_GHOST_TYPE_OPENAI_BACKEND (llm_ghost_openai_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostOpenAIBackend, llm_ghost_openai_backend,
                      LLM_GHOST, OPENAI_BACKEND, GObject)

/**
 * llm_ghost_openai_backend_new:
 * @base_url: API base, e.g. "https://api.openai.com/v1". NULL/"" →
 *            $LLMGHOST_OPENAI_BASE_URL or the OpenAI default.
 * @model:    model id. NULL/"" → $LLMGHOST_OPENAI_MODEL or "" (server default).
 * @api_key:  bearer token. NULL/"" → $LLMGHOST_OPENAI_API_KEY or no auth.
 * @mode:     COMPLETIONS (native FIM) or CHAT (prompt FIM). Overridden by
 *            $LLMGHOST_OPENAI_MODE ("chat"/"completions") when set.
 */
LlmGhostBackend *llm_ghost_openai_backend_new (const char         *base_url,
                                               const char         *model,
                                               const char         *api_key,
                                               LlmGhostOpenAIMode  mode);

G_END_DECLS
