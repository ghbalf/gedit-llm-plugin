#include "llmghost-fim-tokens.h"

#include <string.h>

LlmGhostFimTokens *
llm_ghost_fim_tokens_new (const char         *name,
                          const char         *prefix_tok,
                          const char         *suffix_tok,
                          const char         *middle_tok,
                          const char * const *stop_tokens)
{
  g_return_val_if_fail (name        != NULL, NULL);
  g_return_val_if_fail (prefix_tok  != NULL, NULL);
  g_return_val_if_fail (suffix_tok  != NULL, NULL);
  g_return_val_if_fail (middle_tok  != NULL, NULL);

  LlmGhostFimTokens *self = g_new0 (LlmGhostFimTokens, 1);
  self->name        = g_strdup (name);
  self->prefix_tok  = g_strdup (prefix_tok);
  self->suffix_tok  = g_strdup (suffix_tok);
  self->middle_tok  = g_strdup (middle_tok);
  self->stop_tokens = stop_tokens != NULL
                        ? g_strdupv ((char **) stop_tokens)
                        : g_new0 (char *, 1);
  return self;
}

LlmGhostFimTokens *
llm_ghost_fim_tokens_copy (const LlmGhostFimTokens *self)
{
  if (self == NULL)
    return NULL;
  return llm_ghost_fim_tokens_new (self->name,
                                   self->prefix_tok,
                                   self->suffix_tok,
                                   self->middle_tok,
                                   (const char * const *) self->stop_tokens);
}

void
llm_ghost_fim_tokens_free (LlmGhostFimTokens *self)
{
  if (self == NULL)
    return;
  g_free (self->name);
  g_free (self->prefix_tok);
  g_free (self->suffix_tok);
  g_free (self->middle_tok);
  g_strfreev (self->stop_tokens);
  g_free (self);
}

/* ---- Built-ins ---------------------------------------------------------- */

const LlmGhostFimTokens *
llm_ghost_fim_tokens_qwen (void)
{
  static LlmGhostFimTokens *cached = NULL;
  static gsize init = 0;
  if (g_once_init_enter (&init))
    {
      static const char * const stops[] = {
        "<|endoftext|>", "<|fim_pad|>", "<|im_end|>", NULL,
      };
      cached = llm_ghost_fim_tokens_new ("Qwen",
                                         "<|fim_prefix|>",
                                         "<|fim_suffix|>",
                                         "<|fim_middle|>",
                                         stops);
      g_once_init_leave (&init, 1);
    }
  return cached;
}

const LlmGhostFimTokens *
llm_ghost_fim_tokens_starcoder (void)
{
  static LlmGhostFimTokens *cached = NULL;
  static gsize init = 0;
  if (g_once_init_enter (&init))
    {
      static const char * const stops[] = {
        "<file_sep>", "<|endoftext|>", NULL,
      };
      cached = llm_ghost_fim_tokens_new ("StarCoder",
                                         "<fim_prefix>",
                                         "<fim_suffix>",
                                         "<fim_middle>",
                                         stops);
      g_once_init_leave (&init, 1);
    }
  return cached;
}

const LlmGhostFimTokens *
llm_ghost_fim_tokens_deepseek (void)
{
  static LlmGhostFimTokens *cached = NULL;
  static gsize init = 0;
  if (g_once_init_enter (&init))
    {
      static const char * const stops[] = {
        "<|EOT|>", NULL,
      };
      /* DeepSeek's FIM markers use Unicode codepoints, not ASCII pipes:
       *   U+FF5C  fullwidth vertical line   ｜
       *   U+2581  lower one eighth block    ▁    (SentencePiece word-edge marker)
       * Get a codepoint wrong and the GGUF tokenizer encodes the markers as
       * ordinary subwords; the model then emits them as literal text. */
      cached = llm_ghost_fim_tokens_new ("DeepSeek",
                                         "<｜fim▁begin｜>",
                                         "<｜fim▁hole｜>",
                                         "<｜fim▁end｜>",
                                         stops);
      g_once_init_leave (&init, 1);
    }
  return cached;
}

const LlmGhostFimTokens * const *
llm_ghost_fim_tokens_builtins (void)
{
  static const LlmGhostFimTokens *all[4] = { NULL };
  static gsize init = 0;
  if (g_once_init_enter (&init))
    {
      all[0] = llm_ghost_fim_tokens_qwen ();
      all[1] = llm_ghost_fim_tokens_starcoder ();
      all[2] = llm_ghost_fim_tokens_deepseek ();
      all[3] = NULL;
      g_once_init_leave (&init, 1);
    }
  return all;
}

const LlmGhostFimTokens *
llm_ghost_fim_tokens_lookup_builtin (const char *name)
{
  if (name == NULL)
    return NULL;
  const LlmGhostFimTokens * const *all = llm_ghost_fim_tokens_builtins ();
  for (gsize i = 0; all[i] != NULL; i++)
    if (g_ascii_strcasecmp (all[i]->name, name) == 0)
      return all[i];
  return NULL;
}
