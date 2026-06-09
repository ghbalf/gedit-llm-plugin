#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include "llmghost-generic-backend.h"
#include "llmghost-generic-backend-internal.h"
#include "llmghost-backend.h"

static void
server_cb (SoupServer *server, SoupServerMessage *m, const char *path,
           GHashTable *query, gpointer user_data)
{
  (void) server; (void) path; (void) query; (void) user_data;
  const char *resp =
    "data: {\"choices\":[{\"delta\":{\"content\":\"foo\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"bar\"}}]}\n\n"
    "data: [DONE]\n\n";
  soup_server_message_set_status (m, 200, NULL);
  soup_server_message_set_response (m, "text/event-stream",
                                    SOUP_MEMORY_COPY, resp, strlen (resp));
}

typedef struct { GMainLoop *loop; char *final; GError *error; guint partials; } Run;

static void
on_partial (LlmGhostBackend *b, const char *t, gpointer ud)
{ (void) b; (void) t; ((Run *) ud)->partials++; }

static void
on_done (GObject *src, GAsyncResult *res, gpointer ud)
{
  Run *r = ud;
  r->final = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (src), res, &r->error);
  g_main_loop_quit (r->loop);
}

static void
test_generic_streams (void)
{
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, server_cb, NULL, NULL);
  GError *err = NULL;
  g_assert_true (soup_server_listen_local (server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
  g_assert_no_error (err);
  GSList *uris = soup_server_get_uris (server);
  char *base = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  JsonObject *tmpl = json_object_new ();
  json_object_set_string_member (tmpl, "prompt", "{{prefix}}");
  LlmGhostBackend *b = llm_ghost_generic_backend_new (base, NULL, NULL, tmpl,
                                                      "choices.0.message.content");
  _llm_ghost_generic_backend_set_streaming (LLM_GHOST_GENERIC_BACKEND (b), TRUE,
                                            "choices.0.delta.content", "[DONE]", "stream");

  Run r = { .loop = g_main_loop_new (NULL, FALSE) };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &r);
  llm_ghost_backend_request (b, "pre", "suf", NULL, on_done, &r);
  g_main_loop_run (r.loop);

  g_assert_no_error (r.error);
  g_assert_cmpstr (r.final, ==, "foobar");
  g_assert_cmpuint (r.partials, ==, 2);

  g_free (r.final);
  g_main_loop_unref (r.loop);
  json_object_unref (tmpl);
  g_free (base);
  g_object_unref (b);
  g_object_unref (server);
}

static void
server_cb_multiline (SoupServer *server, SoupServerMessage *m, const char *path,
                     GHashTable *query, gpointer user_data)
{
  (void) server; (void) path; (void) query; (void) user_data;
  const char *resp =
    "data: {\"choices\":[{\"delta\":{\"content\":\"line1\\n\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"line2\"}}]}\n\n"
    "data: [DONE]\n\n";
  soup_server_message_set_status (m, 200, NULL);
  soup_server_message_set_response (m, "text/event-stream",
                                    SOUP_MEMORY_COPY, resp, strlen (resp));
}

static void
test_generic_streams_multiline (void)
{
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, server_cb_multiline, NULL, NULL);
  GError *err = NULL;
  g_assert_true (soup_server_listen_local (server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
  g_assert_no_error (err);
  GSList *uris = soup_server_get_uris (server);
  char *base = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  JsonObject *tmpl = json_object_new ();
  json_object_set_string_member (tmpl, "prompt", "{{prefix}}");
  LlmGhostBackend *b = llm_ghost_generic_backend_new (base, NULL, NULL, tmpl,
                                                      "choices.0.message.content");
  _llm_ghost_generic_backend_set_streaming (LLM_GHOST_GENERIC_BACKEND (b), TRUE,
                                            "choices.0.delta.content", "[DONE]", "stream");
  llm_ghost_generic_backend_set_single_line (LLM_GHOST_GENERIC_BACKEND (b), FALSE);

  Run r = { .loop = g_main_loop_new (NULL, FALSE) };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &r);
  llm_ghost_backend_request (b, "pre", "suf", NULL, on_done, &r);
  g_main_loop_run (r.loop);

  g_assert_no_error (r.error);
  g_assert_cmpstr (r.final, ==, "line1\nline2");   /* multi-line preserved */
  g_assert_cmpuint (r.partials, ==, 2);            /* two non-DONE events */

  g_free (r.final);
  g_main_loop_unref (r.loop);
  json_object_unref (tmpl);
  g_free (base);
  g_object_unref (b);
  g_object_unref (server);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/generic-stream/basic", test_generic_streams);
  g_test_add_func ("/generic-stream/multiline", test_generic_streams_multiline);
  return g_test_run ();
}
