#pragma once

/* Testing-only internal API. NOT part of the installed headers — exists so
 * the unit tests can exercise the otherwise-static request-body builder
 * directly. Do not depend on this from library consumers. */

#include <glib.h>
#include "llmghost-fim-tokens.h"

G_BEGIN_DECLS

char *_llm_ghost_ollama_build_request_body (const char              *model,
                                            const LlmGhostFimTokens *tokens,
                                            const char              *prefix,
                                            const char              *suffix,
                                            guint                    num_predict,
                                            double                   temperature,
                                            gboolean                 single_line);

G_END_DECLS
