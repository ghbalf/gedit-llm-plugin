#pragma once

#include <gtk/gtk.h>

#include "llmghost-backend.h"

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_CONTROLLER (llm_ghost_controller_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostController, llm_ghost_controller,
                      LLM_GHOST, CONTROLLER, GObject)

/**
 * llm_ghost_controller_new:
 * @view:    a #GtkTextView to attach to (not transferred)
 * @backend: backend used to fetch completions (a strong ref is taken)
 *
 * Creates a controller that watches @view's buffer, requests completions
 * from @backend, and renders them as ghost text at the cursor.
 *
 * The returned controller holds a weak reference to @view; when @view is
 * destroyed the controller becomes inert. The caller owns the controller
 * and must g_object_unref() it (typically via g_object_set_data_full on
 * @view or the containing window).
 */
LlmGhostController *llm_ghost_controller_new (GtkTextView     *view,
                                              LlmGhostBackend *backend);

void llm_ghost_controller_set_debounce_ms (LlmGhostController *self,
                                           guint               ms);

G_END_DECLS
