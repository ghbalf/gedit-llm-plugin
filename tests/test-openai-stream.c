#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include "llmghost-openai-backend.h"
#include "llmghost-openai-backend-internal.h"
#include "llmghost-backend.h"

static void
server_cb (SoupServer *server, SoupServerMessage *m, const char *path,
           GHashTable *query, gpointer user_data)
{
  (void) server; (void) path; (void) query; (void) user_data;
  const char *resp =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
    "data: [DONE]\n\n";
  soup_server_message_set_status (m, 200, NULL);
  soup_server_message_set_response (m, "text/event-stream",
                                    SOUP_MEMORY_COPY, resp, strlen (resp));
}

typedef struct {
  GMainLoop *loop;
  char      *final;
  GError    *error;
  guint      partial_count;
  char      *last_partial;
} Run;

static void
on_partial (LlmGhostBackend *b, const char *text, gpointer user_data)
{
  (void) b;
  Run *r = user_data;
  r->partial_count++;
  g_free (r->last_partial);
  r->last_partial = g_strdup (text);
}

static void
on_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  Run *r = user_data;
  r->final = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                               result, &r->error);
  g_main_loop_quit (r->loop);
}

static void
test_openai_streams_chat (void)
{
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, server_cb, NULL, NULL);
  GError *err = NULL;
  g_assert_true (soup_server_listen_local (server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
  g_assert_no_error (err);
  GSList *uris = soup_server_get_uris (server);
  char *base = g_uri_to_string (uris->data);   /* trailing slash */
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  LlmGhostBackend *b = llm_ghost_openai_backend_new (base, "m", NULL,
                                                     LLM_GHOST_OPENAI_MODE_CHAT);
  _llm_ghost_openai_backend_set_stream (LLM_GHOST_OPENAI_BACKEND (b), TRUE);

  Run r = { .loop = g_main_loop_new (NULL, FALSE) };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &r);
  llm_ghost_backend_request (b, "pre", "suf", NULL, on_done, &r);
  g_main_loop_run (r.loop);

  g_assert_no_error (r.error);
  g_assert_cmpstr (r.final, ==, "Hello world");
  g_assert_cmpuint (r.partial_count, ==, 2);
  g_assert_cmpstr (r.last_partial, ==, "Hello world");

  g_free (r.final);
  g_free (r.last_partial);
  g_main_loop_unref (r.loop);
  g_free (base);
  g_object_unref (b);
  g_object_unref (server);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/openai-stream/chat", test_openai_streams_chat);
  return g_test_run ();
}
