#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_SETTINGS (llm_ghost_settings_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostSettings, llm_ghost_settings,
                      LLM_GHOST, SETTINGS, GObject)

/**
 * llm_ghost_settings_new:
 * @path: settings.json path, or %NULL for the XDG default
 *        ($XDG_CONFIG_HOME/llmghost/settings.json).
 *
 * Loads the file (writing a populated default first if it is absent),
 * interpolates ${ENV_VAR} in every string value, and watches the file for
 * live edits. A malformed file falls back to built-in defaults and is left
 * untouched. Emits "changed" when a live edit reloads successfully.
 */
LlmGhostSettings *llm_ghost_settings_new (const char *path);

/* The XDG default settings path. Newly-allocated; free with g_free(). */
char *llm_ghost_settings_default_path (void);

/* Active backend key ("ollama"/"openai"/"mistral"). Never NULL; a missing,
 * empty, or non-string value yields "ollama". Owned by @self, valid until the
 * next reload. */
const char *llm_ghost_settings_get_active_backend (LlmGhostSettings *self);

/* Optional debounce override. Returns TRUE and writes *out_ms when the config
 * sets a positive integer "debounce_ms"; returns FALSE when absent. */
gboolean llm_ghost_settings_get_debounce_ms (LlmGhostSettings *self,
                                             guint            *out_ms);

/* The interpolated params object for backend @name (under "backends"), or
 * %NULL when absent. Owned by @self, valid until the next reload. */
JsonObject *llm_ghost_settings_get_backend_params (LlmGhostSettings *self,
                                                   const char       *name);

/* Rewrite the file at @path with its current contents, to trigger any
 * GFileMonitor watching it (a CHANGES_DONE_HINT) so a keyring change that did
 * not modify the file still causes a live reload + re-interpolation. Content is
 * preserved. Returns FALSE + @error if the file cannot be read or written. */
gboolean llm_ghost_settings_touch (const char *path, GError **error);

G_END_DECLS
