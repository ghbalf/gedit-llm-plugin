#include "llmghost-overlay.h"

struct _LlmGhostOverlay
{
  GtkLabel parent_instance;
};

G_DEFINE_FINAL_TYPE (LlmGhostOverlay, llm_ghost_overlay, GTK_TYPE_LABEL)

static void
llm_ghost_overlay_class_init (LlmGhostOverlayClass *klass)
{
  (void) klass;
}

static void
llm_ghost_overlay_init (LlmGhostOverlay *self)
{
  GtkLabel *label = GTK_LABEL (self);
  GtkWidget *widget = GTK_WIDGET (self);

  gtk_label_set_xalign (label, 0.0f);
  gtk_label_set_yalign (label, 0.0f);
  gtk_label_set_single_line_mode (label, TRUE);
  gtk_label_set_use_markup (label, TRUE);
  gtk_label_set_selectable (label, FALSE);

  gtk_widget_set_can_focus (widget, FALSE);
  gtk_widget_set_sensitive (widget, FALSE);
  gtk_widget_set_no_show_all (widget, TRUE);
}

GtkWidget *
llm_ghost_overlay_new (void)
{
  return g_object_new (LLM_GHOST_TYPE_OVERLAY, NULL);
}

void
llm_ghost_overlay_set_text (LlmGhostOverlay *self,
                            const char      *text)
{
  g_return_if_fail (LLM_GHOST_IS_OVERLAY (self));

  if (text == NULL || *text == '\0')
    {
      gtk_label_set_markup (GTK_LABEL (self), "");
      return;
    }

  /* Truncate at first newline for phase 1 — single-line ghost only. */
  const char *nl = strchr (text, '\n');
  char *line = nl ? g_strndup (text, (size_t) (nl - text)) : g_strdup (text);

  char *escaped = g_markup_escape_text (line, -1);
  char *markup = g_strdup_printf (
      "<span foreground=\"#888888\" style=\"italic\">%s</span>", escaped);

  gtk_label_set_markup (GTK_LABEL (self), markup);

  g_free (markup);
  g_free (escaped);
  g_free (line);
}
