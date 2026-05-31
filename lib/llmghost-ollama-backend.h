#pragma once

#include "llmghost-backend.h"
#include "llmghost-fim-tokens.h"

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_OLLAMA_BACKEND (llm_ghost_ollama_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostOllamaBackend, llm_ghost_ollama_backend,
                      LLM_GHOST, OLLAMA_BACKEND, GObject)

/**
 * llm_ghost_ollama_backend_new:
 * @host:  Ollama host. Pass %NULL or "" for the project default ("spark-2448").
 * @port:  TCP port. Pass 0 for the Ollama default (11434).
 * @model: Model tag. Pass %NULL or "" for the default ("qwen3-coder-next:latest").
 *
 * The backend talks to Ollama's `/api/generate` endpoint in raw FIM mode,
 * wrapping `prefix` and `suffix` with the configured FIM token set. The
 * default token set is Qwen-family — switch with
 * llm_ghost_ollama_backend_set_fim_tokens() when using a model from a
 * different family (StarCoder, DeepSeek, ...).
 */
LlmGhostBackend *llm_ghost_ollama_backend_new (const char *host,
                                               guint16     port,
                                               const char *model);

/**
 * llm_ghost_ollama_backend_set_fim_tokens:
 * @self:   the backend.
 * @tokens: token set to use, or %NULL to reset to the Qwen default.
 *          The struct is deep-copied; the caller retains ownership.
 */
void llm_ghost_ollama_backend_set_fim_tokens (LlmGhostOllamaBackend   *self,
                                              const LlmGhostFimTokens *tokens);

G_END_DECLS
