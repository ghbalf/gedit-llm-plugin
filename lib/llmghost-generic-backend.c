#define G_LOG_DOMAIN "llmghost-generic"

#include "llmghost-generic-backend-internal.h"
#include "llmghost-generic-backend.h"
#include "llmghost-text-util.h"
#include "llmghost-http-util.h"

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
_llm_ghost_generic_build_body (JsonObject *template,
                               const char *prefix,
                               const char *suffix,
                               const char *model)
{
  /* Deep-copy so the stored template is never mutated. */
  JsonNode *wrap = json_node_alloc ();
  json_node_init_object (wrap, template);   /* refs template */
  JsonNode *copy = json_node_copy (wrap);   /* deep copy */
  json_node_unref (wrap);

  substitute_node (copy, prefix, suffix, model);

  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, copy);      /* transfer none */
  char *out = json_generator_to_data (gen, NULL);
  g_object_unref (gen);
  json_node_unref (copy);
  return out;
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

  char *clean = _llm_ghost_clean_single_line (raw);
  g_free (raw);
  g_task_return_pointer (task, clean, g_free);
  g_object_unref (task);
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
