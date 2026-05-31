#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_OVERLAY (llm_ghost_overlay_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostOverlay, llm_ghost_overlay,
                      LLM_GHOST, OVERLAY, GtkLabel)

GtkWidget *llm_ghost_overlay_new      (void);
void       llm_ghost_overlay_set_text (LlmGhostOverlay *self,
                                       const char      *text);

G_END_DECLS
