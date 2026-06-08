#include "llmghost-controller.h"
#include "llmghost-overlay.h"
#include "llmghost-controller-internal.h"

#include <gdk/gdkkeysyms.h>
#include <string.h>

#define DEFAULT_DEBOUNCE_MS 80
#define MAX_CONTEXT_BYTES   (8 * 1024)

struct _LlmGhostController
{
  GObject              parent_instance;

  GtkTextView         *view;       /* weak — nulled by g_object_add_weak_pointer */
  LlmGhostBackend     *backend;    /* strong */
  LlmGhostOverlay     *overlay;    /* strong; child of view's text window when added */
  GCancellable        *cancellable;/* strong; current in-flight, or NULL */

  char                *current_ghost; /* the suggestion text currently displayed */

  guint                debounce_ms;
  guint                debounce_id;     /* GSource id of pending debounce, 0 if none */

  gulong               h_buffer_changed;
  gulong               h_buffer_cursor;
  gulong               h_view_keypress;
  gulong               h_view_destroy;
  gulong               h_backend_partial;
  gulong               h_vadj_changed;
  gulong               h_hadj_changed;

  GtkTextBuffer       *connected_buffer; /* tracks which buffer the buffer-side handlers are on */
  GtkAdjustment       *connected_vadj;
  GtkAdjustment       *connected_hadj;

  guint                overlay_added : 1;
  guint                overlay_visible : 1;
  guint                inserting_acceptance : 1;
};

G_DEFINE_FINAL_TYPE (LlmGhostController, llm_ghost_controller, G_TYPE_OBJECT)

/* ---- forward declarations ------------------------------------------------ */

static void     on_buffer_changed         (GtkTextBuffer *buffer, gpointer user_data);
static void     on_cursor_position_changed (GObject *buffer, GParamSpec *pspec, gpointer user_data);
static gboolean on_view_key_press         (GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void     on_view_destroy           (GtkWidget *widget, gpointer user_data);
static void     on_view_scrolled          (GtkAdjustment *adj, gpointer user_data);
static gboolean on_debounce_fire          (gpointer user_data);
static void     on_completion_ready       (GObject *source, GAsyncResult *result, gpointer user_data);
static void     on_partial_data           (LlmGhostBackend *backend, const char *text, gpointer user_data);
static void     restart_request           (LlmGhostController *self);
static char *   sanitize_ghost_text       (char *text);
static void     show_ghost_at_cursor      (LlmGhostController *self);
static void     reposition_ghost          (LlmGhostController *self);
static void     hide_ghost                (LlmGhostController *self);
static void     accept_ghost              (LlmGhostController *self);
static void     accept_ghost_prefix       (LlmGhostController *self, gsize n_bytes);
static void     cancel_in_flight          (LlmGhostController *self);
static void     clear_debounce            (LlmGhostController *self);
static void     detach_from_view          (LlmGhostController *self);

/* ---- lifecycle ----------------------------------------------------------- */

static void
llm_ghost_controller_init (LlmGhostController *self)
{
  self->debounce_ms = DEFAULT_DEBOUNCE_MS;
  self->overlay = LLM_GHOST_OVERLAY (llm_ghost_overlay_new ());
  g_object_ref_sink (self->overlay);
}

static void
llm_ghost_controller_dispose (GObject *object)
{
  LlmGhostController *self = LLM_GHOST_CONTROLLER (object);

  clear_debounce (self);
  cancel_in_flight (self);
  detach_from_view (self);

  if (self->backend != NULL && self->h_backend_partial != 0)
    {
      g_signal_handler_disconnect (self->backend, self->h_backend_partial);
      self->h_backend_partial = 0;
    }
  g_clear_object (&self->backend);
  g_clear_object (&self->overlay);

  G_OBJECT_CLASS (llm_ghost_controller_parent_class)->dispose (object);
}

static void
llm_ghost_controller_finalize (GObject *object)
{
  LlmGhostController *self = LLM_GHOST_CONTROLLER (object);

  g_clear_pointer (&self->current_ghost, g_free);

  G_OBJECT_CLASS (llm_ghost_controller_parent_class)->finalize (object);
}

static void
llm_ghost_controller_class_init (LlmGhostControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose  = llm_ghost_controller_dispose;
  object_class->finalize = llm_ghost_controller_finalize;
}

/* ---- attachment ---------------------------------------------------------- */

static void
attach_to_view (LlmGhostController *self,
                GtkTextView        *view)
{
  self->view = view;
  g_object_add_weak_pointer (G_OBJECT (view), (gpointer *) &self->view);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
  self->connected_buffer = buffer;
  self->h_buffer_changed = g_signal_connect (buffer, "changed",
                                             G_CALLBACK (on_buffer_changed), self);
  self->h_buffer_cursor  = g_signal_connect (buffer, "notify::cursor-position",
                                             G_CALLBACK (on_cursor_position_changed), self);

  self->h_view_keypress = g_signal_connect (view, "key-press-event",
                                            G_CALLBACK (on_view_key_press), self);
  self->h_view_destroy  = g_signal_connect (view, "destroy",
                                            G_CALLBACK (on_view_destroy), self);

  /* GtkTextView is a GtkScrollable: when packed in a GtkScrolledWindow the
   * adjustments fire value-changed on every scroll. Listen on both axes so
   * the overlay tracks the cursor across scrolls. Adjustments may be NULL
   * if the view isn't yet inside a scrollable container. */
  GtkAdjustment *vadj = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (view));
  GtkAdjustment *hadj = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (view));
  if (vadj != NULL)
    {
      self->connected_vadj = g_object_ref (vadj);
      self->h_vadj_changed = g_signal_connect (vadj, "value-changed",
                                               G_CALLBACK (on_view_scrolled), self);
    }
  if (hadj != NULL)
    {
      self->connected_hadj = g_object_ref (hadj);
      self->h_hadj_changed = g_signal_connect (hadj, "value-changed",
                                               G_CALLBACK (on_view_scrolled), self);
    }
}

static void
detach_from_view (LlmGhostController *self)
{
  if (self->connected_buffer != NULL)
    {
      if (self->h_buffer_changed != 0)
        g_signal_handler_disconnect (self->connected_buffer, self->h_buffer_changed);
      if (self->h_buffer_cursor != 0)
        g_signal_handler_disconnect (self->connected_buffer, self->h_buffer_cursor);
      self->h_buffer_changed = 0;
      self->h_buffer_cursor  = 0;
      self->connected_buffer = NULL;
    }

  if (self->connected_vadj != NULL)
    {
      if (self->h_vadj_changed != 0)
        g_signal_handler_disconnect (self->connected_vadj, self->h_vadj_changed);
      self->h_vadj_changed = 0;
      g_clear_object (&self->connected_vadj);
    }
  if (self->connected_hadj != NULL)
    {
      if (self->h_hadj_changed != 0)
        g_signal_handler_disconnect (self->connected_hadj, self->h_hadj_changed);
      self->h_hadj_changed = 0;
      g_clear_object (&self->connected_hadj);
    }

  if (self->view != NULL)
    {
      if (self->h_view_keypress != 0)
        g_signal_handler_disconnect (self->view, self->h_view_keypress);
      if (self->h_view_destroy != 0)
        g_signal_handler_disconnect (self->view, self->h_view_destroy);
      self->h_view_keypress = 0;
      self->h_view_destroy  = 0;

      if (self->overlay_added)
        {
          gtk_container_remove (GTK_CONTAINER (self->view),
                                GTK_WIDGET (self->overlay));
          self->overlay_added   = FALSE;
          self->overlay_visible = FALSE;
        }

      g_object_remove_weak_pointer (G_OBJECT (self->view),
                                    (gpointer *) &self->view);
      self->view = NULL;
    }
}

LlmGhostController *
llm_ghost_controller_new (GtkTextView     *view,
                          LlmGhostBackend *backend)
{
  g_return_val_if_fail (GTK_IS_TEXT_VIEW (view), NULL);
  g_return_val_if_fail (LLM_GHOST_IS_BACKEND (backend), NULL);

  LlmGhostController *self = g_object_new (LLM_GHOST_TYPE_CONTROLLER, NULL);
  self->backend = g_object_ref (backend);
  self->h_backend_partial =
    g_signal_connect (self->backend, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                      G_CALLBACK (on_partial_data), self);
  attach_to_view (self, view);
  return self;
}

void
llm_ghost_controller_set_debounce_ms (LlmGhostController *self, guint ms)
{
  g_return_if_fail (LLM_GHOST_IS_CONTROLLER (self));
  self->debounce_ms = ms;
}

/* ---- request flow -------------------------------------------------------- */

static void
clear_debounce (LlmGhostController *self)
{
  if (self->debounce_id != 0)
    {
      g_source_remove (self->debounce_id);
      self->debounce_id = 0;
    }
}

static void
cancel_in_flight (LlmGhostController *self)
{
  if (self->cancellable != NULL)
    {
      g_cancellable_cancel (self->cancellable);
      g_clear_object (&self->cancellable);
    }
}

static void
on_view_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  LlmGhostController *self = user_data;
  /* Drop everything tied to the view before GTK tears it down. */
  clear_debounce (self);
  cancel_in_flight (self);
  detach_from_view (self);
}

/* Shared "kick the request pipeline" path. Called from any input that
 * invalidates the current ghost — buffer edits, cursor moves, anything
 * that changes the (prefix, suffix, cursor) tuple. */
static void
restart_request (LlmGhostController *self)
{
  if (self->inserting_acceptance)
    return;

  cancel_in_flight (self);
  hide_ghost (self);

  clear_debounce (self);
  self->debounce_id = g_timeout_add (self->debounce_ms, on_debounce_fire, self);
}

static void
on_buffer_changed (GtkTextBuffer *buffer, gpointer user_data)
{
  (void) buffer;
  restart_request (user_data);
}

static void
on_cursor_position_changed (GObject *buffer, GParamSpec *pspec, gpointer user_data)
{
  (void) buffer;
  (void) pspec;
  /* Arrow keys, mouse-click cursor placement, programmatic move: any of
   * these invalidate where ghost text would belong. Same handling as a
   * buffer edit — debounce and re-fire from the new position. */
  restart_request (user_data);
}

static void
on_view_scrolled (GtkAdjustment *adj, gpointer user_data)
{
  (void) adj;
  LlmGhostController *self = user_data;
  /* Scrolling does not invalidate the ghost; it just moves the cursor's
   * window-coords. Recompute and slide the overlay along. */
  if (self->overlay_visible)
    reposition_ghost (self);
}

/* Slice the last MAX_CONTEXT_BYTES of `s`, on a UTF-8 char boundary. */
static char *
tail_utf8 (const char *s, gsize max_bytes)
{
  gsize len = strlen (s);
  if (len <= max_bytes)
    return g_strdup (s);

  const char *start = s + (len - max_bytes);
  /* advance to next valid UTF-8 leading byte */
  while (*start != '\0' && ((unsigned char) *start & 0xC0) == 0x80)
    start++;
  return g_strdup (start);
}

static char *
head_utf8 (const char *s, gsize max_bytes)
{
  gsize len = strlen (s);
  if (len <= max_bytes)
    return g_strdup (s);

  gsize cut = max_bytes;
  /* back off to UTF-8 boundary */
  while (cut > 0 && ((unsigned char) s[cut] & 0xC0) == 0x80)
    cut--;
  return g_strndup (s, cut);
}

/* Ghost text only makes sense when the rest of the current line (from the
 * cursor to the line end) is empty or whitespace-only. Otherwise the
 * overlay would visually overlap real text. Mirrors the behaviour of
 * Copilot, Supermaven, etc. */
static gboolean
cursor_safe_for_ghost (GtkTextView *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
  GtkTextIter cursor, line_end;
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor,
                                    gtk_text_buffer_get_insert (buffer));

  line_end = cursor;
  if (!gtk_text_iter_ends_line (&line_end))
    gtk_text_iter_forward_to_line_end (&line_end);

  char *rest = gtk_text_buffer_get_text (buffer, &cursor, &line_end, FALSE);
  gboolean safe = TRUE;
  for (const char *p = rest; *p != '\0'; p++)
    {
      if (!g_ascii_isspace ((unsigned char) *p))
        {
          safe = FALSE;
          break;
        }
    }
  g_free (rest);
  return safe;
}

static gboolean
on_debounce_fire (gpointer user_data)
{
  LlmGhostController *self = user_data;
  self->debounce_id = 0;

  if (self->view == NULL)
    return G_SOURCE_REMOVE;

  /* No point requesting a completion we couldn't safely render. */
  if (!cursor_safe_for_ghost (self->view))
    return G_SOURCE_REMOVE;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  GtkTextIter cursor, start, end;
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor,
                                    gtk_text_buffer_get_insert (buffer));
  gtk_text_buffer_get_start_iter (buffer, &start);
  gtk_text_buffer_get_end_iter   (buffer, &end);

  char *prefix_full = gtk_text_buffer_get_text (buffer, &start,  &cursor, FALSE);
  char *suffix_full = gtk_text_buffer_get_text (buffer, &cursor, &end,    FALSE);

  /* Bound the prompt: last N bytes of prefix, first N bytes of suffix. */
  char *prefix = tail_utf8 (prefix_full, MAX_CONTEXT_BYTES);
  char *suffix = head_utf8 (suffix_full, MAX_CONTEXT_BYTES);

  g_free (prefix_full);
  g_free (suffix_full);

  self->cancellable = g_cancellable_new ();
  llm_ghost_backend_request (self->backend,
                             prefix, suffix,
                             self->cancellable,
                             on_completion_ready,
                             g_object_ref (self));

  g_free (prefix);
  g_free (suffix);

  return G_SOURCE_REMOVE;
}

static void
on_completion_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  LlmGhostController *self = user_data;
  GError *error = NULL;

  char *text = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                                 result, &error);

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("llmghost: completion failed: %s", error->message);
      g_clear_error (&error);
      goto out;
    }

  if (self->view == NULL || text == NULL || *text == '\0')
    {
      g_free (text);
      goto out;
    }

  text = sanitize_ghost_text (text);  /* consumes text */
  if (text == NULL)
    goto out;

  g_clear_pointer (&self->current_ghost, g_free);
  self->current_ghost = text;  /* take ownership */

  show_ghost_at_cursor (self);

out:
  g_object_unref (self);
}

/* Phase 2: ghost is a single-line GtkLabel overlay. Truncate @text at the
 * first newline so display and Tab-accept agree (multi-line is phase 3), and
 * right-trim trailing whitespace — FIM models routinely emit it before the
 * stop token, which would render as cursor-on-space after accept. Leading
 * whitespace is preserved (meaningful indentation). Takes ownership of @text;
 * returns the same buffer truncated in place, or NULL (freeing @text) when
 * nothing meaningful remains. */
static char *
sanitize_ghost_text (char *text)
{
  char *nl = strchr (text, '\n');
  if (nl != NULL)
    *nl = '\0';

  for (char *end = text + strlen (text);
       end > text && g_ascii_isspace ((unsigned char) end[-1]);
       end--)
    end[-1] = '\0';

  if (*text == '\0')
    {
      g_free (text);
      return NULL;
    }
  return text;
}

static void
on_partial_data (LlmGhostBackend *backend, const char *text, gpointer user_data)
{
  (void) backend;
  LlmGhostController *self = user_data;

  /* Gate: only render while a request is in flight, so a late emission from a
   * cancelled request can't bleed into a new context. */
  if (self->cancellable == NULL)
    return;
  if (self->view == NULL || text == NULL || *text == '\0')
    return;
  if (!cursor_safe_for_ghost (self->view))
    return;

  /* Same single-line sanitisation as the final result, so the displayed ghost
   * and what Tab-accept inserts agree even mid-stream. */
  char *clean = sanitize_ghost_text (g_strdup (text));
  if (clean == NULL)
    return;

  g_clear_pointer (&self->current_ghost, g_free);
  self->current_ghost = clean;  /* take ownership */
  show_ghost_at_cursor (self);
}

/* ---- rendering ----------------------------------------------------------- */

static void
show_ghost_at_cursor (LlmGhostController *self)
{
  if (self->view == NULL || self->current_ghost == NULL)
    return;

  /* Race guard: the user may have moved the cursor between request and
   * response. Re-check that the position is still safe to render at. */
  if (!cursor_safe_for_ghost (self->view))
    {
      hide_ghost (self);
      return;
    }

  /* GTK 3 quirk: for GTK_TEXT_WINDOW_TEXT children,
   * gtk_text_view_add_child_in_window() / _move_child() store the (x,y)
   * as buffer coordinates internally (`from_top_of_buffer`) and render
   * at `stored - yoffset`. The docs call these "window coordinates" but
   * in practice they are buffer coordinates. So we pass `line_y` and
   * `cursor_rect.x` straight from the validators — no
   * buffer_to_window_coords step. (Doing the conversion would
   * pre-subtract the scroll offset, then GTK subtracts it again at
   * render time → the overlay drifts up by exactly the scroll offset.)
   *
   * get_line_yrange() additionally forces layout validation of the
   * cursor's line, which is necessary for a freshly-typed last line. */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  GtkTextIter cursor_iter;
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor_iter,
                                    gtk_text_buffer_get_insert (buffer));

  gint line_y = 0, line_h = 0;
  gtk_text_view_get_line_yrange (self->view, &cursor_iter, &line_y, &line_h);

  GdkRectangle cursor_rect;
  gtk_text_view_get_cursor_locations (self->view, &cursor_iter, &cursor_rect, NULL);

  llm_ghost_overlay_set_text (self->overlay, self->current_ghost);

  if (!self->overlay_added)
    {
      gtk_text_view_add_child_in_window (self->view,
                                         GTK_WIDGET (self->overlay),
                                         GTK_TEXT_WINDOW_TEXT,
                                         cursor_rect.x, line_y);
      self->overlay_added = TRUE;
    }
  else
    {
      gtk_text_view_move_child (self->view, GTK_WIDGET (self->overlay),
                                cursor_rect.x, line_y);
    }

  gtk_widget_show (GTK_WIDGET (self->overlay));
  self->overlay_visible = TRUE;
}

static void
reposition_ghost (LlmGhostController *self)
{
  if (self->view == NULL || !self->overlay_visible || !self->overlay_added)
    return;

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  GtkTextIter cursor_iter;
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor_iter,
                                    gtk_text_buffer_get_insert (buffer));

  gint line_y = 0, line_h = 0;
  gtk_text_view_get_line_yrange (self->view, &cursor_iter, &line_y, &line_h);

  GdkRectangle cursor_rect;
  gtk_text_view_get_cursor_locations (self->view, &cursor_iter, &cursor_rect, NULL);

  gtk_text_view_move_child (self->view, GTK_WIDGET (self->overlay),
                            cursor_rect.x, line_y);
}

static void
hide_ghost (LlmGhostController *self)
{
  g_clear_pointer (&self->current_ghost, g_free);
  if (self->overlay_visible)
    {
      gtk_widget_hide (GTK_WIDGET (self->overlay));
      self->overlay_visible = FALSE;
    }
}

/* ---- ghost-acceptance boundary helpers (exposed for tests) -------------- */

gsize
_llm_ghost_controller_next_char_len (const char *ghost)
{
  if (ghost == NULL || *ghost == '\0')
    return 0;
  return (gsize) (g_utf8_next_char (ghost) - ghost);
}

gsize
_llm_ghost_controller_next_word_len (const char *ghost)
{
  if (ghost == NULL || *ghost == '\0')
    return 0;

  const char *p = ghost;
  while (*p != '\0' && g_unichar_isspace (g_utf8_get_char (p)))   /* leading whitespace */
    p = g_utf8_next_char (p);

  if (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isalnum (c) || c == '_')                      /* run of word chars */
        {
          while (*p != '\0')
            {
              gunichar w = g_utf8_get_char (p);
              if (!g_unichar_isalnum (w) && w != '_')
                break;
              p = g_utf8_next_char (p);
            }
        }
      else                                                        /* single punctuation char */
        {
          p = g_utf8_next_char (p);
        }
    }

  return (gsize) (p - ghost);
}

/* ---- key handling -------------------------------------------------------- */

/* Accept the first n_bytes of the current ghost: insert that slice, keep the
 * remainder visible (re-rendered at the advanced cursor), or hide if nothing
 * is left. The inserting_acceptance guard stops the buffer insert from
 * triggering restart_request (which would cancel + hide + re-fetch). */
static void
accept_ghost_prefix (LlmGhostController *self, gsize n_bytes)
{
  if (self->view == NULL || self->current_ghost == NULL || n_bytes == 0)
    return;

  n_bytes = MIN (n_bytes, strlen (self->current_ghost));

  char *accepted = g_strndup (self->current_ghost, n_bytes);
  char *rest     = g_strdup (self->current_ghost + n_bytes);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  self->inserting_acceptance = TRUE;
  gtk_text_buffer_insert_at_cursor (buffer, accepted, -1);
  self->inserting_acceptance = FALSE;
  g_free (accepted);

  g_clear_pointer (&self->current_ghost, g_free);

  if (*rest != '\0')
    {
      self->current_ghost = rest;          /* take ownership */
      show_ghost_at_cursor (self);
    }
  else
    {
      g_free (rest);
      hide_ghost (self);
    }
}

static void
accept_ghost (LlmGhostController *self)
{
  if (self->current_ghost != NULL)
    accept_ghost_prefix (self, strlen (self->current_ghost));
}

static gboolean
on_view_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  (void) widget;
  LlmGhostController *self = user_data;

  if (!self->overlay_visible)
    return GDK_EVENT_PROPAGATE;

  guint mods = event->state & gtk_accelerator_get_default_mod_mask ();

  switch (event->keyval)
    {
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
      accept_ghost (self);
      return GDK_EVENT_STOP;

    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      if (mods == 0)
        {
          accept_ghost_prefix (self, _llm_ghost_controller_next_char_len (self->current_ghost));
          return GDK_EVENT_STOP;
        }
      if (mods == GDK_CONTROL_MASK)
        {
          accept_ghost_prefix (self, _llm_ghost_controller_next_word_len (self->current_ghost));
          return GDK_EVENT_STOP;
        }
      return GDK_EVENT_PROPAGATE;

    case GDK_KEY_Escape:
      cancel_in_flight (self);
      hide_ghost (self);
      return GDK_EVENT_STOP;

    default:
      return GDK_EVENT_PROPAGATE;
    }
}
