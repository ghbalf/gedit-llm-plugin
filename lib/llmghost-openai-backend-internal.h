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
                                                double      temperature,
                                                gboolean    stream);

char *_llm_ghost_openai_build_chat_body        (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature,
                                                gboolean    stream);

/* Pull the completion text from a parsed response @root. For CHAT, cleans
 * via _llm_ghost_clean_single_line. Returns "" for no/empty choices; NULL +
 * @error when the body carries an API error object. */
char *_llm_ghost_openai_extract_completion     (JsonNode           *root,
                                                LlmGhostOpenAIMode  mode,
                                                GError            **error);

/* Extract the incremental delta text from one streaming event @event. Returns
 * "" when the event carries no content (role-only opener, finish chunk).
 * chat -> choices[0].delta.content; completions -> choices[0].text. NULL +
 * @error when @event carries an API error object. Newly-allocated. */
char *_llm_ghost_openai_extract_delta          (JsonNode           *event,
                                                LlmGhostOpenAIMode  mode,
                                                GError            **error);

/* Override the default streaming behavior (default: TRUE = stream when able). */
void  _llm_ghost_openai_backend_set_stream     (LlmGhostOpenAIBackend *self,
                                                gboolean               stream);

G_END_DECLS
