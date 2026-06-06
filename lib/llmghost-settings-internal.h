#pragma once

/* Testing-only internal API for LlmGhostSettings. NOT installed. */

#include <glib.h>
#include "llmghost-settings.h"

G_BEGIN_DECLS

/* Expand substitutions in @in and return a newly-allocated result. @in may be
 * NULL (→ ""). Two forms are recognised:
 *   ${NAME}         — replaced with g_getenv("NAME"); warns + "" if unset.
 *   ${secret:NAME}  — resolved via the secret-source seam (libsecret by
 *                     default); warns + "" if unavailable.
 * A literal '$' not followed by '{', or an unclosed "${", is copied verbatim.
 * Substituted values are NOT re-scanned (single left-to-right pass). */
char *_llm_ghost_settings_interpolate (const char *in);

/* Build a settings object from a JSON string with no backing file and no
 * monitor (test seam). Malformed JSON → built-in defaults. */
LlmGhostSettings *_llm_ghost_settings_new_from_string (const char *json);

/* Re-read the backing file and refresh the cache (what the GFileMonitor
 * callback calls). On parse success: update cache + emit "changed". On parse
 * failure or read error: keep the last-good cache and log; no signal. No-op if
 * the object has no backing file. */
void _llm_ghost_settings_reload (LlmGhostSettings *self);

/* Secret-source seam (testing). The interpolator resolves ${secret:NAME} via
 * this function; the default is a libsecret-backed lookup. The fn returns a
 * newly-allocated value (g_free) or NULL when the secret is unavailable (the
 * interpolator then warns and substitutes ""). Pass NULL to restore the
 * default. */
typedef char *(*LlmGhostSecretLookupFn) (const char *name);
void _llm_ghost_settings_set_secret_lookup_for_testing (LlmGhostSecretLookupFn fn);

/* Collect distinct ${secret:NAME} names referenced in any string value of
 * @root (recursing objects + arrays). Returns a newly-allocated,
 * NULL-terminated, de-duplicated array (g_strfreev). Never NULL (may be empty).
 * @root may be NULL (→ empty). Scan the RAW parse, before interpolation. */
char **_llm_ghost_settings_collect_secret_refs (JsonObject *root);

G_END_DECLS
