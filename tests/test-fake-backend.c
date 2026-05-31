#include <glib.h>
#include <gio/gio.h>
#include "llmghost-backend.h"
#include "llmghost-fake-backend.h"

typedef struct {
  GMainLoop *loop;
  char      *result;
  GError    *error;
} Ctx;

static void
on_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Ctx *c = user_data;
  c->result = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                                res, &c->error);
  g_main_loop_quit (c->loop);
}

static char *
run_one_request (LlmGhostBackend *backend, GError **error)
{
  Ctx c = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  llm_ghost_backend_request (backend, "prefix", "suffix", NULL, on_ready, &c);
  g_main_loop_run (c.loop);
  g_main_loop_unref (c.loop);
  if (error != NULL)
    *error = c.error;
  else
    g_clear_error (&c.error);
  return c.result;
}

static void
test_fake_returns_canned (void)
{
  LlmGhostBackend *b = llm_ghost_fake_backend_new ("CANNED");
  GError *error = NULL;
  char *out = run_one_request (b, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "CANNED");
  g_free (out);
  g_object_unref (b);
}

static void
test_fake_default_response (void)
{
  LlmGhostBackend *b = llm_ghost_fake_backend_new (NULL);
  GError *error = NULL;
  char *out = run_one_request (b, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "// hello, ghost!");
  g_free (out);
  g_object_unref (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fake-backend/returns-canned",   test_fake_returns_canned);
  g_test_add_func ("/fake-backend/default-response",  test_fake_default_response);
  return g_test_run ();
}
