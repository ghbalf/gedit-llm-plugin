#pragma once

/* Testing-only internal API for LlmGhostSettings. NOT installed. */

#include <glib.h>
#include "llmghost-settings.h"

G_BEGIN_DECLS

/* Replace each ${NAME} in @in with g_getenv("NAME"). An unset variable expands
 * to "" and logs a warning. A literal '$' not followed by '{', or a "${" with
 * no closing '}', is copied verbatim. @in may be NULL (→ ""). Newly-allocated. */
char *_llm_ghost_settings_interpolate (const char *in);

/* Build a settings object from a JSON string with no backing file and no
 * monitor (test seam). Malformed JSON → built-in defaults. */
LlmGhostSettings *_llm_ghost_settings_new_from_string (const char *json);

/* Re-read the backing file and refresh the cache (what the GFileMonitor
 * callback calls). On parse success: update cache + emit "changed". On parse
 * failure or read error: keep the last-good cache and log; no signal. No-op if
 * the object has no backing file. */
void _llm_ghost_settings_reload (LlmGhostSettings *self);

G_END_DECLS
