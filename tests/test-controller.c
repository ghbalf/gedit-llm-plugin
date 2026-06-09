#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "llmghost-controller.h"
#include "llmghost-overlay.h"
#include "mock-backend.h"

#define DEBOUNCE_MS 30
#define SETTLE_MS   90   /* > debounce, leaves room for idle dispatch */

static gboolean stop_loop (gpointer loop) { g_main_loop_quit (loop); return G_SOURCE_REMOVE; }

static void
pump (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (ms, stop_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

typedef struct {
  GtkWidget          *window;
  GtkWidget          *scrolled;
  GtkTextView        *view;
  LlmGhostBackend    *backend;
  LlmGhostController *controller;
} Fixture;

static Fixture *
fixture_new (void)
{
  Fixture *f = g_new0 (Fixture, 1);
  f->window   = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (f->window), 300, 200);
  f->scrolled = gtk_scrolled_window_new (NULL, NULL);
  f->view     = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_container_add (GTK_CONTAINER (f->scrolled), GTK_WIDGET (f->view));
  gtk_container_add (GTK_CONTAINER (f->window), f->scrolled);

  f->backend = mock_backend_new ();
  mock_backend_set_response (MOCK_BACKEND (f->backend), "abc");
  f->controller = llm_ghost_controller_new (f->view, f->backend);
  llm_ghost_controller_set_debounce_ms (f->controller, DEBOUNCE_MS);

  gtk_widget_show_all (f->window);
  pump (SETTLE_MS);
  return f;
}

static void
fixture_free (Fixture *f)
{
  g_object_unref (f->controller);
  g_object_unref (f->backend);
  gtk_widget_destroy (f->window);
  /* Drain any deferred GTask completions (e.g. the CANCELLED idle scheduled by
   * the controller's dispose) so nothing dangles past the test. */
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_free (f);
}

static GtkTextBuffer *buf (Fixture *f) { return gtk_text_view_get_buffer (f->view); }

static char *
buffer_text (Fixture *f)
{
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds (buf (f), &s, &e);
  return gtk_text_buffer_get_text (buf (f), &s, &e, FALSE);
}

static LlmGhostOverlay *
find_ghost_overlay (Fixture *f)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (f->view));
  LlmGhostOverlay *found = NULL;
  /* The inline first-line overlay is the single-line-mode one; the block
   * (multi-line) overlay must not be confused for it. */
  for (GList *l = children; l != NULL; l = l->next)
    if (LLM_GHOST_IS_OVERLAY (l->data) &&
        gtk_label_get_single_line_mode (GTK_LABEL (l->data)))
      {
        found = l->data;
        break;
      }
  g_list_free (children);
  return found;
}

static gboolean
ghost_visible (Fixture *f)
{
  LlmGhostOverlay *o = find_ghost_overlay (f);
  return o != NULL && gtk_widget_get_visible (GTK_WIDGET (o));
}

static char *
ghost_text (Fixture *f)
{
  LlmGhostOverlay *o = find_ghost_overlay (f);
  if (o == NULL)
    return g_strdup ("");
  return g_strdup (gtk_label_get_text (GTK_LABEL (o)));
}

static char *
block_text (Fixture *f)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (f->view));
  char *out = g_strdup ("");
  guint seen = 0;
  for (GList *l = children; l != NULL; l = l->next)
    if (LLM_GHOST_IS_OVERLAY (l->data))
      {
        /* Two overlays may be present: inline (single-line) and block
         * (multi-line). The block is the one whose label text contains a
         * newline, or — when the block is a single continuation line — the
         * one that is NOT single-line-mode. Identify by single-line-mode. */
        if (!gtk_label_get_single_line_mode (GTK_LABEL (l->data)))
          {
            g_free (out);
            out = g_strdup (gtk_label_get_text (GTK_LABEL (l->data)));
            seen++;
          }
      }
  g_list_free (children);
  /* At most one block overlay should ever exist; duplicates would mean the
   * teardown/recreate path leaked a previous block widget. */
  g_assert_cmpuint (seen, <=, 1);
  return out;
}

/* TRUE if some text tag opens a vertical gap (pixels-below-lines > 0) at the
 * start of the line containing the cursor — i.e. the spacer gap is applied. */
static gboolean
cursor_line_has_gap (GtkTextBuffer *buffer)
{
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark (buffer, &it,
                                    gtk_text_buffer_get_insert (buffer));
  gtk_text_iter_set_line_offset (&it, 0);
  gboolean gap = FALSE;
  GSList *tags = gtk_text_iter_get_tags (&it);
  for (GSList *l = tags; l != NULL; l = l->next)
    {
      gint below = 0;
      g_object_get (l->data, "pixels-below-lines", &below, NULL);
      if (below > 0)
        {
          gap = TRUE;
          break;
        }
    }
  g_slist_free (tags);
  return gap;
}

static gboolean
send_key_mod (Fixture *f, guint keyval, guint state)
{
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.keyval = keyval;
  ev->key.state  = state;
  GdkWindow *win = gtk_widget_get_window (GTK_WIDGET (f->view));
  ev->key.window = win != NULL ? g_object_ref (win) : NULL;
  gboolean handled = FALSE;
  g_signal_emit_by_name (f->view, "key-press-event", ev, &handled);
  gdk_event_free (ev);
  return handled;
}

static gboolean
send_key (Fixture *f, guint keyval)
{
  return send_key_mod (f, keyval, 0);
}

static void
test_debounce_coalesces (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "a", -1);
  gtk_text_buffer_insert_at_cursor (buf (f), "b", -1);
  gtk_text_buffer_insert_at_cursor (buf (f), "c", -1);
  gtk_text_buffer_insert_at_cursor (buf (f), "d", -1);

  pump (SETTLE_MS);

  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 1);
  fixture_free (f);
}

static void
test_cancel_on_new_input (void)
{
  Fixture *f = fixture_new ();

  gtk_text_buffer_insert_at_cursor (buf (f), "a", -1);
  pump (SETTLE_MS);
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 1);

  gtk_text_buffer_insert_at_cursor (buf (f), "b", -1);
  pump (SETTLE_MS);

  g_assert_cmpint (mock_backend_cancel_count  (MOCK_BACKEND (f->backend)), ==, 1);
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 2);

  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));
  fixture_free (f);
}

static void
test_tab_accepts (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Tab));

  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "fabc");
  g_free (text);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_escape_dismisses (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Escape));

  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "f");
  g_free (text);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_right_accepts_char (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));   /* response "abc" */
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Right));
  char *t1 = buffer_text (f);
  g_assert_cmpstr (t1, ==, "fa");
  g_free (t1);
  g_assert_true (ghost_visible (f));   /* "bc" remains */

  g_assert_true (send_key (f, GDK_KEY_Right));
  char *t2 = buffer_text (f);
  g_assert_cmpstr (t2, ==, "fab");
  g_free (t2);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Right));
  char *t3 = buffer_text (f);
  g_assert_cmpstr (t3, ==, "fabc");
  g_free (t3);
  g_assert_false (ghost_visible (f));  /* nothing left */
  fixture_free (f);
}

static void
test_ctrl_right_accepts_word (void)
{
  Fixture *f = fixture_new ();
  mock_backend_set_response (MOCK_BACKEND (f->backend), "foo bar");
  gtk_text_buffer_insert_at_cursor (buf (f), "x", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t1 = buffer_text (f);
  g_assert_cmpstr (t1, ==, "xfoo");
  g_free (t1);
  g_assert_true (ghost_visible (f));   /* " bar" remains */

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t2 = buffer_text (f);
  g_assert_cmpstr (t2, ==, "xfoo bar");
  g_free (t2);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_ctrl_right_punctuation (void)
{
  Fixture *f = fixture_new ();
  mock_backend_set_response (MOCK_BACKEND (f->backend), "ab(c");
  gtk_text_buffer_insert_at_cursor (buf (f), "x", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t1 = buffer_text (f);
  g_assert_cmpstr (t1, ==, "xab");      /* word run "ab" */
  g_free (t1);

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t2 = buffer_text (f);
  g_assert_cmpstr (t2, ==, "xab(");     /* single punctuation "(" */
  g_free (t2);

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t3 = buffer_text (f);
  g_assert_cmpstr (t3, ==, "xab(c");    /* word run "c" */
  g_free (t3);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_midline_suppression (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_set_text (buf (f), "xy", -1);
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (buf (f), &start);
  gtk_text_buffer_place_cursor (buf (f), &start);

  pump (SETTLE_MS);

  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 0);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_sanity_coords_after_scroll (void)
{
  Fixture *f = fixture_new ();
  gtk_widget_set_size_request (f->scrolled, 220, 120);

  GString *s = g_string_new (NULL);
  for (int i = 0; i < 60; i++)
    g_string_append_printf (s, "line %d\n", i);
  gtk_text_buffer_set_text (buf (f), s->str, -1);
  g_string_free (s, TRUE);

  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buf (f), &end);
  gtk_text_buffer_place_cursor (buf (f), &end);
  GtkTextMark *insert = gtk_text_buffer_get_insert (buf (f));
  gtk_text_view_scroll_to_mark (f->view, insert, 0.0, TRUE, 0.0, 0.5);
  pump (SETTLE_MS);

  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);

  LlmGhostOverlay *overlay = find_ghost_overlay (f);
  g_assert_nonnull (overlay);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (overlay)));

  GtkAdjustment *vadj =
      gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (f->view));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark (buf (f), &cur, insert);
  gint line_y = 0, line_h = 0;
  gtk_text_view_get_line_yrange (f->view, &cur, &line_y, &line_h);
  g_assert_cmpint (line_h, >, 0);
  g_assert_cmpint ((gint) gtk_adjustment_get_value (vadj), >, line_h);

  GdkRectangle r;
  gtk_text_view_get_iter_location (f->view, &cur, &r);
  gint win_x = 0, win_y = 0;
  gtk_text_view_buffer_to_window_coords (f->view, GTK_TEXT_WINDOW_TEXT,
                                         r.x, r.y, &win_x, &win_y);

  GtkAllocation alloc;
  gtk_widget_get_allocation (GTK_WIDGET (overlay), &alloc);

  g_assert_cmpint (ABS (alloc.y - win_y), <=, line_h);
  fixture_free (f);
}

static void
test_partial_renders_incrementally (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);   /* request now in-flight (parked in mock) */

  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "He");
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));
  char *t1 = ghost_text (f);
  g_assert_cmpstr (t1, ==, "He");
  g_free (t1);

  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "Hello");
  pump (SETTLE_MS);
  char *t2 = ghost_text (f);
  g_assert_cmpstr (t2, ==, "Hello");
  g_free (t2);

  fixture_free (f);
}

static void
test_partial_gated_when_idle (void)
{
  Fixture *f = fixture_new ();
  /* No request in flight (cancellable == NULL): a stray partial must not show. */
  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "ghost");
  pump (SETTLE_MS);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

/* A multi-line partial now renders as a block and accepts in full. */
static void
test_partial_accept_multi_line (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);   /* request now in-flight */

  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "line1\nline2");
  pump (SETTLE_MS);

  g_assert_true (send_key (f, GDK_KEY_Tab));
  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "fline1\nline2");
  g_free (text);
  fixture_free (f);
}

static void
test_multiline_splits_inline_and_block (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "foo(\n    bar,\n    baz");
  pump (SETTLE_MS);

  g_assert_true (ghost_visible (f));
  char *inline_t = ghost_text (f);
  g_assert_cmpstr (inline_t, ==, "foo(");          /* first line inline */
  g_free (inline_t);

  char *blk = block_text (f);
  g_assert_cmpstr (blk, ==, "    bar,\n    baz");   /* lines 2..N in the block */
  g_free (blk);
  fixture_free (f);
}

static void
test_multiline_cap (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  /* 10 lines, cap is 8 → accept inserts exactly 8 (first line "L1" continues
   * the buffer's "f"). */
  mock_backend_emit_partial (MOCK_BACKEND (f->backend),
                             "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
  pump (SETTLE_MS);
  g_assert_true (send_key (f, GDK_KEY_Tab));
  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "fL1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
  g_free (text);
  fixture_free (f);
}

/* Detaching the controller while a multi-line ghost is visible must remove the
 * spacer gap from the surviving buffer — otherwise a blank gap is left behind
 * (the plugin recreates the controller per-view on settings reload). */
static void
test_detach_clears_spacer (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "line1\nline2");
  pump (SETTLE_MS);

  /* Keep the buffer alive past view destruction. */
  GtkTextBuffer *buffer = g_object_ref (buf (f));

  /* Precondition: the multi-line spacer gap is actually applied right now. */
  g_assert_true (cursor_line_has_gap (buffer));

  /* Destroying the view (Option B) fires on_view_destroy → detach_from_view
   * without going through hide_ghost. We use this rather than disposing the
   * controller because an in-flight request still holds a ref on the
   * controller (the mock emitted a partial, not a completion), so unref'ing
   * the controller would not dispose it. The view owns no extra controller
   * ref, so its destroy reliably drives the detach path under test. */
  gtk_widget_destroy (GTK_WIDGET (f->view));

  g_assert_false (cursor_line_has_gap (buffer));

  /* Teardown: controller/backend/window are still alive. fixture_free is safe
   * here (it does not touch the now-destroyed view directly), but the view
   * pointer is dangling, so tear down explicitly instead. */
  g_object_unref (buffer);
  g_object_unref (f->controller);
  g_object_unref (f->backend);
  gtk_widget_destroy (f->window);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_free (f);
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  g_test_add_func ("/controller/debounce-coalesces",   test_debounce_coalesces);
  g_test_add_func ("/controller/cancel-on-new-input",  test_cancel_on_new_input);
  g_test_add_func ("/controller/tab-accepts",          test_tab_accepts);
  g_test_add_func ("/controller/escape-dismisses",     test_escape_dismisses);
  g_test_add_func ("/controller/right-accepts-char",      test_right_accepts_char);
  g_test_add_func ("/controller/ctrl-right-accepts-word", test_ctrl_right_accepts_word);
  g_test_add_func ("/controller/ctrl-right-punctuation",  test_ctrl_right_punctuation);
  g_test_add_func ("/controller/midline-suppression",  test_midline_suppression);
  g_test_add_func ("/controller/sanity-coords",        test_sanity_coords_after_scroll);
  g_test_add_func ("/controller/partial-incremental", test_partial_renders_incrementally);
  g_test_add_func ("/controller/partial-gated-idle",  test_partial_gated_when_idle);
  g_test_add_func ("/controller/partial-accept-multi-line", test_partial_accept_multi_line);
  g_test_add_func ("/controller/multiline-split", test_multiline_splits_inline_and_block);
  g_test_add_func ("/controller/multiline-cap",   test_multiline_cap);
  g_test_add_func ("/controller/detach-clears-spacer", test_detach_clears_spacer);
  return g_test_run ();
}
