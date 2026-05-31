#include <glib.h>
#include <gio/gio.h>
#include "mock-backend.h"

typedef struct {
  GMainLoop *loop;
  char      *result;
  GError    *error;
  gboolean   done;
} Ctx;

static void
on_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Ctx *c = user_data;
  c->result = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                                res, &c->error);
  c->done = TRUE;
  if (c->loop != NULL)
    g_main_loop_quit (c->loop);
}

/* Pump the default context briefly so parked GTask completions dispatch. */
static gboolean stop_loop (gpointer loop) { g_main_loop_quit (loop); return G_SOURCE_REMOVE; }
static void
pump (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (ms, stop_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);
}

static void
test_complete_returns_response (void)
{
  LlmGhostBackend *b = mock_backend_new ();
  mock_backend_set_response (MOCK_BACKEND (b), "HELLO");
  Ctx c = { 0 };

  llm_ghost_backend_request (b, "p", "s", NULL, on_ready, &c);
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (b)), ==, 1);
  g_assert_false (c.done);   /* deferred: nothing yet */

  mock_backend_complete_pending (MOCK_BACKEND (b));
  pump (20);                 /* let the GTask callback dispatch */

  g_assert_true (c.done);
  g_assert_no_error (c.error);
  g_assert_cmpstr (c.result, ==, "HELLO");
  g_free (c.result);
  g_object_unref (b);
}

static void
test_cancel_counts_and_errors (void)
{
  LlmGhostBackend *b = mock_backend_new ();
  GCancellable *cancellable = g_cancellable_new ();
  Ctx c = { 0 };

  llm_ghost_backend_request (b, "p", "s", cancellable, on_ready, &c);
  g_cancellable_cancel (cancellable);   /* fires on_cancelled synchronously */

  g_assert_cmpint (mock_backend_cancel_count (MOCK_BACKEND (b)), ==, 1);
  pump (20);                            /* dispatch the CANCELLED completion */

  g_assert_true (c.done);
  g_assert_error (c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (c.result);
  g_clear_error (&c.error);
  g_object_unref (cancellable);
  g_object_unref (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mock-backend/complete-returns-response", test_complete_returns_response);
  g_test_add_func ("/mock-backend/cancel-counts-and-errors",  test_cancel_counts_and_errors);
  return g_test_run ();
}
