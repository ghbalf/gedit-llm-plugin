#define G_LOG_DOMAIN "llmghost-generic"

#include "llmghost-generic-backend-internal.h"
#include "llmghost-generic-backend.h"
#include "llmghost-text-util.h"
#include "llmghost-http-util.h"
#include "llmghost-backend-internal.h"

#include <string.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

/* ---- template substitution --------------------------------------------- */

/* Replace {{prefix}}/{{suffix}}/{{model}} in @in. Single left-to-right pass;
 * inserted values are never re-scanned. Unknown {{tokens}} copied verbatim.
 * NULL values count as "". Newly-allocated. */
static char *
substitute (const char *in, const char *prefix, const char *suffix, const char *model)
{
  GString *out = g_string_new (NULL);
  const char *p = in;
  while (*p != '\0')
    {
      if (p[0] == '{' && p[1] == '{')
        {
          const char *end = strstr (p + 2, "}}");
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              const char *val = NULL;
              gboolean known = TRUE;
              if (strcmp (name, "prefix") == 0)      val = prefix;
              else if (strcmp (name, "suffix") == 0) val = suffix;
              else if (strcmp (name, "model") == 0)  val = model;
              else                                   known = FALSE;
              g_free (name);
              if (known)
                {
                  g_string_append (out, val != NULL ? val : "");
                  p = end + 2;
                  continue;
                }
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

/* Recurse @node, replacing every string value in place. Handles object members
 * and array elements uniformly via json_node_set_string (json-glib has no
 * array-element setter, so we mutate the element node directly). */
static void
substitute_node (JsonNode *node, const char *prefix, const char *suffix, const char *model)
{
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      JsonObject *obj = json_node_get_object (node);
      GList *members = json_object_get_members (obj);
      for (GList *l = members; l != NULL; l = l->next)
        substitute_node (json_object_get_member (obj, l->data), prefix, suffix, model);
      g_list_free (members);
    }
  else if (JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *arr = json_node_get_array (node);
      guint n = json_array_get_length (arr);
      for (guint i = 0; i < n; i++)
        substitute_node (json_array_get_element (arr, i), prefix, suffix, model);
    }
  else if (JSON_NODE_HOLDS_VALUE (node) &&
           json_node_get_value_type (node) == G_TYPE_STRING)
    {
      char *sub = substitute (json_node_get_string (node), prefix, suffix, model);
      json_node_set_string (node, sub);
      g_free (sub);
    }
}

char *
_llm_ghost_generic_build_body_with_stream (JsonObject *template,
                                           const char *prefix,
                                           const char *suffix,
                                           const char *model,
                                           const char *stream_field,
                                           gboolean    stream_value)
{
  /* Deep-copy so the stored template is never mutated. json_node_copy() on an
   * object node is shallow in json-glib < 1.10 (it shares the same JsonObject),
   * so serialize-and-reparse to get a fully independent tree. */
  JsonNode *wrap = json_node_alloc ();
  json_node_init_object (wrap, template);   /* refs template */
  JsonGenerator *cgen = json_generator_new ();
  json_generator_set_root (cgen, wrap);
  char *serialized = json_generator_to_data (cgen, NULL);
  g_object_unref (cgen);
  json_node_unref (wrap);

  JsonNode *copy = json_from_string (serialized, NULL);
  g_free (serialized);

  /* A serialized JsonObject always reparses as an object; guard anyway so a
   * future non-object template can never crash here. */
  if (copy == NULL || !JSON_NODE_HOLDS_OBJECT (copy))
    {
      g_clear_pointer (&copy, json_node_unref);
      return g_strdup ("{}");
    }

  substitute_node (copy, prefix, suffix, model);

  if (stream_field != NULL && *stream_field != '\0')
    {
      JsonObject *obj = json_node_get_object (copy);
      json_object_set_boolean_member (obj, stream_field, stream_value);
    }

  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, copy);      /* transfer none */
  char *out = json_generator_to_data (gen, NULL);
  g_object_unref (gen);
  json_node_unref (copy);
  return out;
}

char *
_llm_ghost_generic_build_body (JsonObject *template,
                               const char *prefix,
                               const char *suffix,
                               const char *model)
{
  return _llm_ghost_generic_build_body_with_stream (template, prefix, suffix,
                                                    model, NULL, FALSE);
}

/* ---- response-path extraction ------------------------------------------ */

static gboolean
seg_is_index (const char *seg)
{
  if (*seg == '\0')
    return FALSE;
  for (const char *p = seg; *p != '\0'; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  return TRUE;
}

char *
_llm_ghost_generic_extract (JsonNode *root, const char *path, GError **error)
{
  if (root == NULL || path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: null response or response_path");
      return NULL;
    }

  char **segs = g_strsplit (path, ".", -1);
  JsonNode *cur = root;

  for (int i = 0; segs[i] != NULL; i++)
    {
      const char *seg = segs[i];
      if (seg_is_index (seg))
        {
          if (!JSON_NODE_HOLDS_ARRAY (cur))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path segment \"%s\" expected an array", seg);
              g_strfreev (segs);
              return NULL;
            }
          JsonArray *arr = json_node_get_array (cur);
          guint64 idx = g_ascii_strtoull (seg, NULL, 10);
          if (idx >= json_array_get_length (arr))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path index %s out of range", seg);
              g_strfreev (segs);
              return NULL;
            }
          cur = json_array_get_element (arr, (guint) idx);
        }
      else
        {
          if (!JSON_NODE_HOLDS_OBJECT (cur))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path segment \"%s\" expected an object", seg);
              g_strfreev (segs);
              return NULL;
            }
          JsonObject *obj = json_node_get_object (cur);
          if (!json_object_has_member (obj, seg))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path member \"%s\" not found", seg);
              g_strfreev (segs);
              return NULL;
            }
          cur = json_object_get_member (obj, seg);
        }
    }

  g_strfreev (segs);

  if (cur == NULL || !JSON_NODE_HOLDS_VALUE (cur) ||
      json_node_get_value_type (cur) != G_TYPE_STRING)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path did not resolve to a string");
      return NULL;
    }

  return g_strdup (json_node_get_string (cur));
}

char *
_llm_ghost_generic_extract_delta (JsonNode *event, const char *path)
{
  char *s = _llm_ghost_generic_extract (event, path, NULL);
  return s != NULL ? s : g_strdup ("");
}

/* ---- type --------------------------------------------------------------- */

#define REQUEST_TIMEOUT_SEC 30

struct _LlmGhostGenericBackend
{
  GObject      parent_instance;

  SoupSession *session;
  char        *url;
  JsonObject  *headers;            /* owned ref, or NULL */
  char        *model;              /* or NULL */
  JsonObject  *request_template;   /* owned ref, or NULL */
  char        *response_path;

  gboolean  stream;          /* gate (default TRUE) */
  gboolean  single_line;     /* truncate to first line (default TRUE) */
  char     *stream_path;     /* dotted path to per-event delta, or NULL */
  char     *done_marker;     /* sentinel payload to skip (default "[DONE]") */
  char     *stream_field;    /* body member to set to stream value, or NULL */
};

static void llm_ghost_generic_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostGenericBackend, llm_ghost_generic_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_generic_backend_iface_init))

/* ---- request flow ------------------------------------------------------- */

static void
on_http_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask  *task  = G_TASK (user_data);
  GError *error = NULL;

  JsonNode *root = _llm_ghost_http_post_json_finish (result, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  LlmGhostGenericBackend *self = g_task_get_source_object (task);
  char *raw = _llm_ghost_generic_extract (root, self->response_path, &error);
  json_node_unref (root);
  if (raw == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  char *clean = _llm_ghost_clean_text (raw, self->single_line);
  g_free (raw);
  g_task_return_pointer (task, clean, g_free);
  g_object_unref (task);
}

typedef struct {
  LlmGhostGenericBackend *self;   /* borrowed; outer task holds a ref */
  GString                *acc;
  const char             *stream_path;   /* borrowed from self */
  const char             *done_marker;   /* borrowed from self */
  gboolean                single_line;
} GenericStreamCtx;

static void
generic_stream_ctx_free (gpointer data)
{
  GenericStreamCtx *c = data;
  g_string_free (c->acc, TRUE);
  g_free (c);
}

static void
generic_on_event (const char *payload, gpointer user_data)
{
  GenericStreamCtx *ctx = user_data;
  if (g_strcmp0 (payload, ctx->done_marker) == 0)
    return;

  JsonNode *node = _llm_ghost_http_parse_json (payload);
  if (node == NULL)
    return;

  char *delta = _llm_ghost_generic_extract_delta (node, ctx->stream_path);
  json_node_unref (node);
  if (*delta != '\0')
    {
      g_string_append (ctx->acc, delta);
      char *clean = _llm_ghost_clean_text (ctx->acc->str, ctx->single_line);
      _llm_ghost_backend_emit_partial_data (LLM_GHOST_BACKEND (ctx->self), clean);
      g_free (clean);
    }
  g_free (delta);
}

static void
generic_on_stream_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask *outer = user_data;
  GenericStreamCtx *ctx = g_task_get_task_data (outer);
  GError *error = NULL;

  if (!_llm_ghost_http_post_json_stream_finish (result, &error))
    {
      g_task_return_error (outer, error);
      g_object_unref (outer);
      return;
    }

  char *out = _llm_ghost_clean_text (ctx->acc->str, ctx->single_line);
  g_task_return_pointer (outer, out, g_free);
  g_object_unref (outer);
}

static void
generic_request (LlmGhostBackend     *backend,
                 const char          *prefix,
                 const char          *suffix,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
  LlmGhostGenericBackend *self = LLM_GHOST_GENERIC_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  if (self->request_template == NULL || self->url == NULL || *self->url == '\0' ||
      self->response_path == NULL || *self->response_path == '\0')
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "generic backend: incomplete configuration");
      g_object_unref (task);
      return;
    }

  if (self->stream && self->stream_path != NULL && *self->stream_path != '\0')
    {
      GenericStreamCtx *ctx = g_new0 (GenericStreamCtx, 1);
      ctx->self        = self;
      ctx->acc         = g_string_new (NULL);
      ctx->stream_path = self->stream_path;
      ctx->done_marker = self->done_marker;
      ctx->single_line = self->single_line;
      g_task_set_task_data (task, ctx, generic_stream_ctx_free);

      char *body = _llm_ghost_generic_build_body_with_stream (
        self->request_template, prefix, suffix, self->model,
        self->stream_field, TRUE);

      _llm_ghost_http_post_json_stream_async (self->session, self->url,
                                              self->headers, body,
                                              generic_on_event, ctx, cancellable,
                                              generic_on_stream_done, task);
      return;
    }

  char *body = _llm_ghost_generic_build_body (self->request_template,
                                              prefix, suffix, self->model);
  _llm_ghost_http_post_json_headers_async (self->session, self->url, self->headers,
                                           body, cancellable, on_http_done, task);
}

static char *
generic_request_finish (LlmGhostBackend *backend, GAsyncResult *result, GError **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_generic_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = generic_request;
  iface->request_finish = generic_request_finish;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llm_ghost_generic_backend_finalize (GObject *object)
{
  LlmGhostGenericBackend *self = LLM_GHOST_GENERIC_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->url, g_free);
  g_clear_pointer (&self->headers, json_object_unref);
  g_clear_pointer (&self->model, g_free);
  g_clear_pointer (&self->request_template, json_object_unref);
  g_clear_pointer (&self->response_path, g_free);
  g_clear_pointer (&self->stream_path,  g_free);
  g_clear_pointer (&self->done_marker,  g_free);
  g_clear_pointer (&self->stream_field, g_free);
  G_OBJECT_CLASS (llm_ghost_generic_backend_parent_class)->finalize (object);
}

static void
llm_ghost_generic_backend_class_init (LlmGhostGenericBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_generic_backend_finalize;
}

static void
llm_ghost_generic_backend_init (LlmGhostGenericBackend *self)
{
  self->session = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
  self->stream       = TRUE;
  self->single_line  = TRUE;
  self->done_marker  = g_strdup ("[DONE]");
  self->stream_field = g_strdup ("stream");
}

/* ---- construction ------------------------------------------------------- */

LlmGhostBackend *
llm_ghost_generic_backend_new (const char *url,
                               JsonObject *headers,
                               const char *model,
                               JsonObject *request_template,
                               const char *response_path)
{
  LlmGhostGenericBackend *self = g_object_new (LLM_GHOST_TYPE_GENERIC_BACKEND, NULL);

  self->url              = g_strdup (url);
  self->headers          = headers          ? json_object_ref (headers)          : NULL;
  self->model            = g_strdup (model);
  self->request_template = request_template ? json_object_ref (request_template) : NULL;
  self->response_path    = g_strdup (response_path);

  if (url == NULL || *url == '\0')
    g_warning ("generic backend: no \"url\" configured; requests will fail");
  if (request_template == NULL)
    g_warning ("generic backend: no \"request_template\" configured; requests will fail");
  if (response_path == NULL || *response_path == '\0')
    g_warning ("generic backend: no \"response_path\" configured; requests will fail");

  return LLM_GHOST_BACKEND (self);
}

void
_llm_ghost_generic_backend_set_streaming (LlmGhostGenericBackend *self,
                                          gboolean    stream,
                                          const char *stream_path,
                                          const char *done_marker,
                                          const char *stream_field)
{
  g_return_if_fail (LLM_GHOST_IS_GENERIC_BACKEND (self));
  self->stream = stream;

  g_clear_pointer (&self->stream_path, g_free);
  self->stream_path = (stream_path != NULL && *stream_path != '\0')
                        ? g_strdup (stream_path) : NULL;

  if (done_marker != NULL && *done_marker != '\0')
    {
      g_clear_pointer (&self->done_marker, g_free);
      self->done_marker = g_strdup (done_marker);
    }

  /* NULL -> keep default "stream"; explicit "" -> disable body mutation. */
  if (stream_field != NULL)
    {
      g_clear_pointer (&self->stream_field, g_free);
      self->stream_field = g_strdup (stream_field);
    }
}

void
llm_ghost_generic_backend_set_single_line (LlmGhostGenericBackend *self,
                                           gboolean                single_line)
{
  g_return_if_fail (LLM_GHOST_IS_GENERIC_BACKEND (self));
  self->single_line = single_line;
}
