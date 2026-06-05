#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "llmghost-http-util.h"

/* ---- in-process test server -------------------------------------------- */

typedef struct {
  char *last_auth;          /* captured Authorization header ("" if none) */
  char *last_content_type;  /* captured Content-Type */
  char *last_body;          /* captured request body */
  char *last_xapikey;       /* captured x-api-key header ("" if none) */
  char *last_anthropic_ver; /* captured anthropic-version header ("" if none) */
} Captured;

static void
server_cb (SoupServer *server, SoupServerMessage *m, const char *path,
           GHashTable *query, gpointer user_data)
{
  (void) server; (void) query;
  Captured *cap = user_data;

  SoupMessageHeaders *h = soup_server_message_get_request_headers (m);
  const char *auth = soup_message_headers_get_one (h, "Authorization");
  const char *ct   = soup_message_headers_get_one (h, "Content-Type");
  g_clear_pointer (&cap->last_auth, g_free);
  g_clear_pointer (&cap->last_content_type, g_free);
  g_clear_pointer (&cap->last_body, g_free);
  cap->last_auth = g_strdup (auth ? auth : "");
  cap->last_content_type = g_strdup (ct ? ct : "");

  const char *xak = soup_message_headers_get_one (h, "x-api-key");
  const char *av  = soup_message_headers_get_one (h, "anthropic-version");
  g_clear_pointer (&cap->last_xapikey, g_free);
  g_clear_pointer (&cap->last_anthropic_ver, g_free);
  cap->last_xapikey      = g_strdup (xak ? xak : "");
  cap->last_anthropic_ver = g_strdup (av ? av : "");

  SoupMessageBody *body = soup_server_message_get_request_body (m);
  cap->last_body = g_strndup (body->data ? body->data : "", body->length);

  if (g_strcmp0 (path, "/ok") == 0)
    {
      const char *resp = "{\"hello\":\"world\"}";
      soup_server_message_set_status (m, 200, NULL);
      soup_server_message_set_response (m, "application/json",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
  else if (g_strcmp0 (path, "/bad") == 0)
    {
      const char *resp = "kaboom";
      soup_server_message_set_status (m, 500, NULL);
      soup_server_message_set_response (m, "text/plain",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
  else /* /malformed */
    {
      const char *resp = "not json {";
      soup_server_message_set_status (m, 200, NULL);
      soup_server_message_set_response (m, "application/json",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
}

typedef struct {
  SoupServer *server;
  char       *base;       /* e.g. "http://127.0.0.1:PORT/" */
  Captured    cap;
} Srv;

static Srv *
srv_new (void)
{
  Srv *s = g_new0 (Srv, 1);
  s->server = soup_server_new (NULL, NULL);
  soup_server_add_handler (s->server, NULL, server_cb, &s->cap, NULL);

  GError *error = NULL;
  g_assert_true (soup_server_listen_local (s->server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error (error);

  GSList *uris = soup_server_get_uris (s->server);
  g_assert_nonnull (uris);
  s->base = g_uri_to_string (uris->data);   /* trailing slash included */
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  return s;
}

static void
srv_free (Srv *s)
{
  g_clear_object (&s->server);
  g_free (s->base);
  g_free (s->cap.last_auth);
  g_free (s->cap.last_content_type);
  g_free (s->cap.last_body);
  g_free (s->cap.last_xapikey);
  g_free (s->cap.last_anthropic_ver);
  g_free (s);
}

/* ---- async driver ------------------------------------------------------ */

typedef struct {
  GMainLoop *loop;
  JsonNode  *node;
  GError    *error;
} Wait;

static void
on_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  Wait *w = user_data;
  w->node = _llm_ghost_http_post_json_finish (result, &w->error);
  g_main_loop_quit (w->loop);
}

static JsonNode *
post_headers (Srv *s, const char *path, JsonObject *headers, const char *body, GError **error)
{
  SoupSession *session = soup_session_new ();
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  Wait w = { .loop = loop };
  char *url = g_strconcat (s->base, path + 1, NULL);

  _llm_ghost_http_post_json_headers_async (session, url, headers, g_strdup (body),
                                           NULL, on_done, &w);
  g_main_loop_run (loop);

  g_free (url);
  g_main_loop_unref (loop);
  g_object_unref (session);
  if (error != NULL) *error = w.error; else g_clear_error (&w.error);
  return w.node;
}

static JsonNode *
post (Srv *s, const char *path, const char *bearer, const char *body, GError **error)
{
  SoupSession *session = soup_session_new ();
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  Wait w = { .loop = loop };
  char *url = g_strconcat (s->base, path + 1, NULL);  /* base ends in '/', path starts '/' */

  _llm_ghost_http_post_json_async (session, url, bearer, g_strdup (body),
                                   NULL, on_done, &w);
  g_main_loop_run (loop);

  g_free (url);
  g_main_loop_unref (loop);
  g_object_unref (session);
  if (error != NULL)
    *error = w.error;
  else
    g_clear_error (&w.error);
  return w.node;
}

/* ---- tests ------------------------------------------------------------- */

static void
test_ok_with_bearer (void)
{
  Srv *s = srv_new ();
  GError *error = NULL;
  JsonNode *node = post (s, "/ok", "secret", "{\"x\":1}", &error);

  g_assert_no_error (error);
  g_assert_nonnull (node);
  JsonObject *obj = json_node_get_object (node);
  g_assert_cmpstr (json_object_get_string_member (obj, "hello"), ==, "world");

  g_assert_cmpstr (s->cap.last_auth, ==, "Bearer secret");
  g_assert_true (g_str_has_prefix (s->cap.last_content_type, "application/json"));
  g_assert_cmpstr (s->cap.last_body, ==, "{\"x\":1}");

  json_node_unref (node);
  srv_free (s);
}

static void
test_ok_without_bearer (void)
{
  Srv *s = srv_new ();
  JsonNode *node = post (s, "/ok", NULL, "{}", NULL);
  g_assert_nonnull (node);
  g_assert_cmpstr (s->cap.last_auth, ==, "");   /* no Authorization header */
  json_node_unref (node);
  srv_free (s);
}

static void
test_http_500_is_error (void)
{
  Srv *s = srv_new ();
  GError *error = NULL;
  JsonNode *node = post (s, "/bad", NULL, "{}", &error);
  g_assert_null (node);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (error->message, -1, "500"));    /* status in message */
  g_assert_nonnull (g_strstr_len (error->message, -1, "kaboom")); /* snippet in message */
  g_clear_error (&error);
  srv_free (s);
}

static void
test_malformed_json_is_error (void)
{
  Srv *s = srv_new ();
  GError *error = NULL;
  JsonNode *node = post (s, "/malformed", NULL, "{}", &error);
  g_assert_null (node);
  g_assert_nonnull (error);
  g_clear_error (&error);
  srv_free (s);
}

static void
test_custom_headers (void)
{
  Srv *s = srv_new ();
  JsonObject *headers = json_object_new ();
  json_object_set_string_member (headers, "x-api-key", "secret-key");
  json_object_set_string_member (headers, "anthropic-version", "2023-06-01");

  JsonNode *node = post_headers (s, "/ok", headers, "{\"x\":1}", NULL);
  g_assert_nonnull (node);

  g_assert_cmpstr (s->cap.last_xapikey,       ==, "secret-key");
  g_assert_cmpstr (s->cap.last_anthropic_ver, ==, "2023-06-01");
  g_assert_cmpstr (s->cap.last_auth,          ==, "");                 /* no Bearer */
  g_assert_true (g_str_has_prefix (s->cap.last_content_type, "application/json"));

  json_node_unref (node);
  json_object_unref (headers);
  srv_free (s);
}

static void
test_bearer_wrapper_still_works (void)
{
  /* The reimplemented Bearer call must still send Authorization through the
   * new core. */
  Srv *s = srv_new ();
  JsonNode *node = post (s, "/ok", "tok", "{}", NULL);
  g_assert_nonnull (node);
  g_assert_cmpstr (s->cap.last_auth, ==, "Bearer tok");
  json_node_unref (node);
  srv_free (s);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/http-util/ok-with-bearer",    test_ok_with_bearer);
  g_test_add_func ("/http-util/ok-without-bearer", test_ok_without_bearer);
  g_test_add_func ("/http-util/http-500",          test_http_500_is_error);
  g_test_add_func ("/http-util/malformed-json",    test_malformed_json_is_error);
  g_test_add_func ("/http-util/custom-headers",  test_custom_headers);
  g_test_add_func ("/http-util/bearer-wrapper",  test_bearer_wrapper_still_works);
  return g_test_run ();
}
