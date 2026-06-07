#define G_LOG_DOMAIN "llmghost-http-util"

#include "llmghost-http-util.h"
#include "llmghost-sse-parser.h"
#include <string.h>

static void
on_soup_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask       *task    = G_TASK (user_data);
  SoupSession *session = SOUP_SESSION (source);
  SoupMessage *msg     = SOUP_MESSAGE (g_task_get_task_data (task));
  GError      *error   = NULL;

  GBytes *body = soup_session_send_and_read_finish (session, result, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  guint       status = soup_message_get_status (msg);
  gsize       len    = 0;
  const char *data   = body ? g_bytes_get_data (body, &len) : NULL;

  if (status < 200 || status >= 300)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "HTTP %u: %.*s", status,
                               (int) MIN (len, 256u), data ? data : "");
      g_clear_pointer (&body, g_bytes_unref);
      g_object_unref (task);
      return;
    }

  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data ? data : "", (gssize) len, &error))
    {
      g_task_return_error (task, error);
      g_object_unref (parser);
      g_clear_pointer (&body, g_bytes_unref);
      g_object_unref (task);
      return;
    }

  JsonNode *root = json_parser_get_root (parser);
  JsonNode *copy = root ? json_node_copy (root) : NULL;
  g_object_unref (parser);
  g_clear_pointer (&body, g_bytes_unref);

  if (copy == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "empty JSON response");
      g_object_unref (task);
      return;
    }

  g_task_return_pointer (task, copy, (GDestroyNotify) json_node_unref);
  g_object_unref (task);
}

/* Build a POST SoupMessage for @url with @headers + JSON @json_body. Consumes
 * @json_body in all paths. Returns NULL + @error (G_IO_ERROR_INVALID_ARGUMENT)
 * on an invalid URL. */
static SoupMessage *
make_post_message (const char *url, JsonObject *headers,
                   char *json_body, GError **error)
{
  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  if (msg == NULL)
    {
      g_free (json_body);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "invalid URL: %s", url ? url : "(null)");
      return NULL;
    }

  const char *content_type = "application/json";
  SoupMessageHeaders *h = soup_message_get_request_headers (msg);
  if (headers != NULL)
    {
      JsonObjectIter iter;
      const char *name;
      JsonNode *val;
      json_object_iter_init (&iter, headers);
      while (json_object_iter_next (&iter, &name, &val))
        {
          if (!JSON_NODE_HOLDS_VALUE (val) ||
              json_node_get_value_type (val) != G_TYPE_STRING)
            {
              g_warning ("header \"%s\" is not a string; skipping", name);
              continue;
            }
          if (g_ascii_strcasecmp (name, "Content-Type") == 0)
            {
              content_type = json_node_get_string (val);
              continue;
            }
          soup_message_headers_append (h, name, json_node_get_string (val));
        }
    }

  GBytes *bytes = g_bytes_new_take (json_body, strlen (json_body));
  soup_message_set_request_body_from_bytes (msg, content_type, bytes);
  g_bytes_unref (bytes);
  return msg;
}

void
_llm_ghost_http_post_json_headers_async (SoupSession         *session,
                                         const char          *url,
                                         JsonObject          *headers,
                                         char                *json_body,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GTask *task = g_task_new (session, cancellable, callback, user_data);
  GError *error = NULL;
  SoupMessage *msg = make_post_message (url, headers, json_body, &error);
  if (msg == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  /* Keep the SoupMessage alive until the handler reads its status. */
  g_task_set_task_data (task, msg, g_object_unref);
  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_soup_response, task);
}

void
_llm_ghost_http_post_json_async (SoupSession         *session,
                                 const char          *url,
                                 const char          *bearer,
                                 char                *json_body,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  JsonObject *headers = NULL;
  if (bearer != NULL && *bearer != '\0')
    {
      char *auth = g_strdup_printf ("Bearer %s", bearer);
      headers = json_object_new ();
      json_object_set_string_member (headers, "Authorization", auth);
      g_free (auth);
    }

  /* The core consumes @json_body and reads @headers synchronously before the
   * async send returns, so unref-ing headers right after the call is safe. */
  _llm_ghost_http_post_json_headers_async (session, url, headers, json_body,
                                           cancellable, callback, user_data);
  if (headers != NULL)
    json_object_unref (headers);
}

JsonNode *
_llm_ghost_http_post_json_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

JsonNode *
_llm_ghost_http_parse_json (const char *text)
{
  if (text == NULL || *text == '\0')
    return NULL;
  JsonParser *parser = json_parser_new ();
  JsonNode *copy = NULL;
  if (json_parser_load_from_data (parser, text, -1, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL)
        copy = json_node_copy (root);
    }
  g_object_unref (parser);
  return copy;
}

/* ---- streaming transport ----------------------------------------------- */

typedef struct {
  SoupMessage        *msg;          /* owned */
  GInputStream       *stream;       /* owned (set after send) */
  LlmGhostSseParser  *parser;       /* owned */
  GPtrArray          *events;       /* scratch, owned */
  LlmGhostSseEventFn  on_event;     /* borrowed */
  gpointer            event_data;   /* borrowed */
  guint               status;
  gboolean            is_error;     /* non-2xx: accumulate body for the message */
  GString            *errbuf;       /* owned when is_error */
  char                buf[4096];
} StreamCtx;

static void
stream_ctx_free (gpointer data)
{
  StreamCtx *c = data;
  g_clear_object (&c->msg);
  g_clear_object (&c->stream);
  if (c->parser != NULL)
    _llm_ghost_sse_parser_free (c->parser);
  if (c->events != NULL)
    g_ptr_array_unref (c->events);
  if (c->errbuf != NULL)
    g_string_free (c->errbuf, TRUE);
  g_free (c);
}

static void
flush_events (StreamCtx *ctx)
{
  for (guint i = 0; i < ctx->events->len; i++)
    ctx->on_event (g_ptr_array_index (ctx->events, i), ctx->event_data);
  g_ptr_array_set_size (ctx->events, 0);   /* frees elements (free func set) */
}

static void read_chunk (GTask *task);

static void
on_read (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GTask *task = user_data;
  StreamCtx *ctx = g_task_get_task_data (task);
  GError *error = NULL;

  gssize n = g_input_stream_read_finish (ctx->stream, res, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  if (n == 0)   /* EOF */
    {
      if (ctx->is_error)
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "HTTP %u: %.*s", ctx->status,
                                 (int) MIN (ctx->errbuf->len, 256u),
                                 ctx->errbuf->str);
      else
        {
          _llm_ghost_sse_parser_finish (ctx->parser, ctx->events);
          flush_events (ctx);
          g_task_return_boolean (task, TRUE);
        }
      g_object_unref (task);
      return;
    }

  if (ctx->is_error)
    g_string_append_len (ctx->errbuf, ctx->buf, n);
  else
    {
      _llm_ghost_sse_parser_feed (ctx->parser, ctx->buf, (gsize) n, ctx->events);
      flush_events (ctx);
    }

  read_chunk (task);
}

static void
read_chunk (GTask *task)
{
  StreamCtx *ctx = g_task_get_task_data (task);
  g_input_stream_read_async (ctx->stream, ctx->buf, sizeof ctx->buf,
                             G_PRIORITY_DEFAULT, g_task_get_cancellable (task),
                             on_read, task);
}

static void
on_send_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  StreamCtx *ctx = g_task_get_task_data (task);
  GError *error = NULL;

  GInputStream *stream =
    soup_session_send_finish (SOUP_SESSION (source), res, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  ctx->stream = stream;   /* owned */
  ctx->status = soup_message_get_status (ctx->msg);
  if (ctx->status < 200 || ctx->status >= 300)
    {
      ctx->is_error = TRUE;
      ctx->errbuf = g_string_new (NULL);
    }
  read_chunk (task);
}

void
_llm_ghost_http_post_json_stream_async (SoupSession         *session,
                                        const char          *url,
                                        JsonObject          *headers,
                                        char                *json_body,
                                        LlmGhostSseEventFn   on_event,
                                        gpointer             event_data,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  GTask *task = g_task_new (session, cancellable, callback, user_data);
  GError *error = NULL;
  SoupMessage *msg = make_post_message (url, headers, json_body, &error);
  if (msg == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  StreamCtx *ctx = g_new0 (StreamCtx, 1);
  ctx->msg        = msg;   /* owned */
  ctx->parser     = _llm_ghost_sse_parser_new ();
  ctx->events     = g_ptr_array_new_with_free_func (g_free);
  ctx->on_event   = on_event;
  ctx->event_data = event_data;
  g_task_set_task_data (task, ctx, stream_ctx_free);

  soup_session_send_async (session, msg, G_PRIORITY_DEFAULT,
                           cancellable, on_send_ready, task);
}

gboolean
_llm_ghost_http_post_json_stream_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
