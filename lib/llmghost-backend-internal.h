#pragma once

/* Internal (NOT installed). Helper for streaming backends to emit the
 * interface's "partial-data" signal. */

#include "llmghost-backend.h"

G_BEGIN_DECLS

/* Emit "partial-data" on @self carrying the accumulated completion text. */
void _llm_ghost_backend_emit_partial_data (LlmGhostBackend *self,
                                           const char      *accumulated);

G_END_DECLS
