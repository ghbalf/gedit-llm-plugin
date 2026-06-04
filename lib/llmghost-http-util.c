#include "llmghost-http-util.h"
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

void
_llm_ghost_http_post_json_async (SoupSession         *session,
                                 const char          *url,
                                 const char          *bearer,
                                 char                *json_body,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GTask *task = g_task_new (session, cancellable, callback, user_data);

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  if (msg == NULL)
    {
      g_free (json_body);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "invalid URL: %s", url ? url : "(null)");
      g_object_unref (task);
      return;
    }

  if (bearer != NULL && *bearer != '\0')
    {
      SoupMessageHeaders *h = soup_message_get_request_headers (msg);
      char *auth = g_strdup_printf ("Bearer %s", bearer);
      soup_message_headers_append (h, "Authorization", auth);
      g_free (auth);
    }

  GBytes *bytes = g_bytes_new_take (json_body, strlen (json_body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  g_bytes_unref (bytes);

  /* Keep the SoupMessage alive until the handler reads its status. */
  g_task_set_task_data (task, msg, g_object_unref);

  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_soup_response, task);
}

JsonNode *
_llm_ghost_http_post_json_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
