#pragma once

/* Testing-only internal API. NOT installed. Pure request-body builders,
 * chat-response cleanup, and response extraction for direct unit testing. */

#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-openai-backend.h"

G_BEGIN_DECLS

char *_llm_ghost_openai_build_completions_body (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature);

char *_llm_ghost_openai_build_chat_body        (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature);

/* Trim, unwrap a single ``` fence, then truncate at the first newline.
 * NULL-safe; always returns a newly-allocated string (possibly ""). */
char *_llm_ghost_openai_clean_chat_completion  (const char *raw);

/* Pull the completion text from a parsed response @root. For CHAT, cleans
 * via the above. Returns "" for no/empty choices; NULL + @error when the
 * body carries an API error object. */
char *_llm_ghost_openai_extract_completion     (JsonNode           *root,
                                                LlmGhostOpenAIMode  mode,
                                                GError            **error);

G_END_DECLS
