#pragma once

/* Testing-only internal API. NOT part of the installed headers — exists so
 * the unit tests can exercise the otherwise-static request-body builder
 * directly. Do not depend on this from library consumers. */

#include <glib.h>
#include "llmghost-fim-tokens.h"
#include "llmghost-ollama-backend.h"

G_BEGIN_DECLS

char *_llm_ghost_ollama_build_request_body (const char              *model,
                                            const LlmGhostFimTokens *tokens,
                                            const char              *prefix,
                                            const char              *suffix,
                                            guint                    num_predict,
                                            double                   temperature,
                                            gboolean                 single_line);

/* Testing-only: read back the single_line flag (set by the factory from the
 * top-level max_lines setting) to cover the single_line == (max_lines == 1)
 * derivation at the factory seam. */
gboolean _llm_ghost_ollama_backend_get_single_line (LlmGhostOllamaBackend *self);

G_END_DECLS
