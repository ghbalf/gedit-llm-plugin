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
  for (GList *l = children; l != NULL; l = l->next)
    if (LLM_GHOST_IS_OVERLAY (l->data))
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
  return g_test_run ();
}
