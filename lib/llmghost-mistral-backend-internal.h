#pragma once

/* Testing-only internal API. NOT installed. Pure FIM request-body builder and
 * response extractor for direct unit testing. */

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

char *_llm_ghost_mistral_build_fim_body    (const char *model,
                                            const char *prefix,
                                            const char *suffix,
                                            guint       max_tokens,
                                            double      temperature,
                                            gboolean    single_line);

/* Pull the completion from a parsed Codestral FIM response @root: prefer
 * choices[0].message.content, fall back to choices[0].text. Returns "" for
 * no/empty choices; NULL + @error when the body carries an API error object. */
char *_llm_ghost_mistral_extract_completion (JsonNode  *root,
                                            GError    **error);

G_END_DECLS
