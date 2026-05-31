#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * LlmGhostFimTokens:
 * @name:        Human-readable identifier ("Qwen", "StarCoder", ...).
 * @prefix_tok:  Sentinel before the prefix, e.g. `<|fim_prefix|>`.
 * @suffix_tok:  Sentinel before the suffix.
 * @middle_tok:  Sentinel marking where generation begins.
 * @stop_tokens: NULL-terminated extra stop strings the backend should send
 *               to the model (newline is added by the backend itself).
 *
 * One model family's FIM convention as a value type. Not a GObject — copy
 * with llm_ghost_fim_tokens_copy(), free with llm_ghost_fim_tokens_free().
 *
 * Three builtins are shipped (Qwen, StarCoder, DeepSeek). Callers that
 * need a different convention can construct one with
 * llm_ghost_fim_tokens_new() — useful for the eventual settings dialog.
 */
typedef struct _LlmGhostFimTokens LlmGhostFimTokens;

struct _LlmGhostFimTokens
{
  char  *name;
  char  *prefix_tok;
  char  *suffix_tok;
  char  *middle_tok;
  char **stop_tokens;
};

LlmGhostFimTokens *llm_ghost_fim_tokens_new   (const char         *name,
                                               const char         *prefix_tok,
                                               const char         *suffix_tok,
                                               const char         *middle_tok,
                                               const char * const *stop_tokens);
LlmGhostFimTokens *llm_ghost_fim_tokens_copy  (const LlmGhostFimTokens *self);
void               llm_ghost_fim_tokens_free  (LlmGhostFimTokens       *self);

/* Built-ins. Owned by the library; do not free or mutate. */
const LlmGhostFimTokens *llm_ghost_fim_tokens_qwen      (void);
const LlmGhostFimTokens *llm_ghost_fim_tokens_starcoder (void);
const LlmGhostFimTokens *llm_ghost_fim_tokens_deepseek  (void);

/* NULL-terminated list of all builtins, in display order. */
const LlmGhostFimTokens * const *llm_ghost_fim_tokens_builtins (void);

/* Case-insensitive name lookup against the builtins. NULL if unknown. */
const LlmGhostFimTokens *llm_ghost_fim_tokens_lookup_builtin (const char *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (LlmGhostFimTokens, llm_ghost_fim_tokens_free)

G_END_DECLS
