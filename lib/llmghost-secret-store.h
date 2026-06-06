#pragma once

/* libsecret-backed secret storage, keyed by a short NAME. Internal (NOT added
 * to installed llmghost_headers); used by the settings interpolator, the prefs
 * dialog, and tests. All calls are synchronous. */

#include <glib.h>

G_BEGIN_DECLS

/* Look up the secret stored under @name. Returns a newly-allocated value
 * (free with g_free), or NULL if not found or on error (then *error is set
 * only on a real error, not on "not found"). */
char     *llm_ghost_secret_lookup (const char *name, GError **error);

/* Store/overwrite @value under @name in the default collection. */
gboolean  llm_ghost_secret_store  (const char *name, const char *value, GError **error);

/* Remove the secret under @name. Removing an absent key is success (idempotent). */
gboolean  llm_ghost_secret_clear  (const char *name, GError **error);

G_END_DECLS
