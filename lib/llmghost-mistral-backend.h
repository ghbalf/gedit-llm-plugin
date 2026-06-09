#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_MISTRAL_BACKEND (llm_ghost_mistral_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostMistralBackend, llm_ghost_mistral_backend,
                      LLM_GHOST, MISTRAL_BACKEND, GObject)

/**
 * llm_ghost_mistral_backend_new:
 * @base_url: API base. NULL/"" → $LLMGHOST_MISTRAL_BASE_URL or the Codestral default.
 * @model:    model id. NULL/"" → $LLMGHOST_MISTRAL_MODEL or "codestral-latest".
 * @api_key:  bearer token. NULL/"" → $LLMGHOST_MISTRAL_API_KEY or no auth.
 *
 * Talks to Mistral's Codestral FIM endpoint (POST {base}/fim/completions),
 * sending the prefix as `prompt` and the suffix as `suffix`.
 */
LlmGhostBackend *llm_ghost_mistral_backend_new (const char *base_url,
                                                const char *model,
                                                const char *api_key);

void llm_ghost_mistral_backend_set_single_line (LlmGhostMistralBackend *self,
                                                gboolean                single_line);

G_END_DECLS
