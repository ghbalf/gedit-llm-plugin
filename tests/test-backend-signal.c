#include <glib.h>
#include "llmghost-backend.h"
#include "llmghost-backend-internal.h"
#include "mock-backend.h"

typedef struct { char *last; guint count; } Seen;

static void
on_partial (LlmGhostBackend *b, const char *text, gpointer user_data)
{
  (void) b;
  Seen *s = user_data;
  g_free (s->last);
  s->last = g_strdup (text);
  s->count++;
}

static void
test_emit_partial_data (void)
{
  LlmGhostBackend *b = mock_backend_new ();
  Seen seen = { 0 };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &seen);

  _llm_ghost_backend_emit_partial_data (b, "hel");
  _llm_ghost_backend_emit_partial_data (b, "hello");

  g_assert_cmpuint (seen.count, ==, 2);
  g_assert_cmpstr (seen.last, ==, "hello");

  g_free (seen.last);
  g_object_unref (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/backend/emit-partial-data", test_emit_partial_data);
  return g_test_run ();
}
