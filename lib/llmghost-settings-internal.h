#pragma once

/* Testing-only internal API for LlmGhostSettings. NOT installed.
 * Grows across the plan; Task 1 declares only the pure interpolation helper. */

#include <glib.h>

G_BEGIN_DECLS

/* Replace each ${NAME} in @in with g_getenv("NAME"). An unset variable expands
 * to "" and logs a warning. A literal '$' not followed by '{', or a "${" with
 * no closing '}', is copied verbatim. @in may be NULL (→ ""). Newly-allocated. */
char *_llm_ghost_settings_interpolate (const char *in);

G_END_DECLS
