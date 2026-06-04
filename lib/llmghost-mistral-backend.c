#include "llmghost-mistral-backend.h"
#include "llmghost-mistral-backend-internal.h"

#include <string.h>
#include <libsoup/soup.h>
#include "llmghost-http-util.h"

/* ---- request body builder ----------------------------------------------- */

char *
_llm_ghost_mistral_build_fim_body (const char *model,
                                   const char *prefix,
                                   const char *suffix,
                                   guint       max_tokens,
                                   double      temperature)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, model ? model : "");
  json_builder_set_member_name (b, "prompt");
  json_builder_add_string_value (b, prefix ? prefix : "");
  json_builder_set_member_name (b, "suffix");
  json_builder_add_string_value (b, suffix ? suffix : "");
  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, max_tokens);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, temperature);
  json_builder_set_member_name (b, "stop");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "\n");
  json_builder_end_array (b);

  json_builder_end_object (b);

  JsonGenerator *gen  = json_generator_new ();
  JsonNode      *root = json_builder_get_root (b);   /* transfer full */
  json_generator_set_root (gen, root);               /* transfer none */
  char *out = json_generator_to_data (gen, NULL);
  json_node_unref (root);
  g_object_unref (gen);
  g_object_unref (b);
  return out;
}

/* ---- response extraction ------------------------------------------------ */

char *
_llm_ghost_mistral_extract_completion (JsonNode *root, GError **error)
{
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "mistral: malformed response");
      return NULL;
    }

  JsonObject *obj = json_node_get_object (root);

  if (json_object_has_member (obj, "error"))
    {
      JsonNode   *en  = json_object_get_member (obj, "error");
      const char *msg = NULL;
      if (JSON_NODE_HOLDS_OBJECT (en))
        {
          JsonObject *eo = json_node_get_object (en);
          if (json_object_has_member (eo, "message"))
            msg = json_object_get_string_member (eo, "message");
        }
      else if (JSON_NODE_HOLDS_VALUE (en))
        {
          msg = json_node_get_string (en);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "mistral: %s", msg ? msg : "(error)");
      return NULL;
    }

  if (!json_object_has_member (obj, "choices"))
    return g_strdup ("");

  JsonArray *choices = json_object_get_array_member (obj, "choices");
  if (choices == NULL || json_array_get_length (choices) == 0)
    return g_strdup ("");

  JsonObject *choice = json_array_get_object_element (choices, 0);

  /* Codestral FIM returns a chat-style choice; tolerate a plain-text shape. */
  if (json_object_has_member (choice, "message"))
    {
      JsonObject *m = json_object_get_object_member (choice, "message");
      if (m != NULL && json_object_has_member (m, "content"))
        {
          const char *content = json_object_get_string_member (m, "content");
          return g_strdup (content ? content : "");
        }
    }

  if (json_object_has_member (choice, "text"))
    {
      const char *text = json_object_get_string_member (choice, "text");
      return g_strdup (text ? text : "");
    }

  return g_strdup ("");
}

/* ---- type --------------------------------------------------------------- */

#define DEFAULT_BASE_URL     "https://codestral.mistral.ai/v1"
#define DEFAULT_MODEL        "codestral-latest"
#define DEFAULT_MAX_TOKENS   64
#define DEFAULT_TEMPERATURE  0.2
#define REQUEST_TIMEOUT_SEC  30

struct _LlmGhostMistralBackend
{
  GObject       parent_instance;

  SoupSession  *session;
  char         *base_url;
  char         *model;
  char         *api_key;     /* NULL = no auth */
  guint         max_tokens;
  double        temperature;
};

static void llm_ghost_mistral_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostMistralBackend, llm_ghost_mistral_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_mistral_backend_iface_init))

/* ---- request flow ------------------------------------------------------- */

static char *
join_url (const char *base, const char *endpoint)
{
  gsize n = strlen (base);
  if (n > 0 && base[n - 1] == '/')
    return g_strconcat (base, endpoint, NULL);
  return g_strconcat (base, "/", endpoint, NULL);
}

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

  char *out = _llm_ghost_mistral_extract_completion (root, &error);
  json_node_unref (root);

  if (out == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  g_task_return_pointer (task, out, g_free);
  g_object_unref (task);
}

static void
mistral_request (LlmGhostBackend     *backend,
                 const char          *prefix,
                 const char          *suffix,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
  LlmGhostMistralBackend *self = LLM_GHOST_MISTRAL_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  char *url  = join_url (self->base_url, "fim/completions");
  char *body = _llm_ghost_mistral_build_fim_body (self->model, prefix, suffix,
                                                  self->max_tokens, self->temperature);

  _llm_ghost_http_post_json_async (self->session, url, self->api_key, body,
                                   cancellable, on_http_done, task);
  g_free (url);
}

static char *
mistral_request_finish (LlmGhostBackend *backend, GAsyncResult *result, GError **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_mistral_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = mistral_request;
  iface->request_finish = mistral_request_finish;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llm_ghost_mistral_backend_finalize (GObject *object)
{
  LlmGhostMistralBackend *self = LLM_GHOST_MISTRAL_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->model,    g_free);
  g_clear_pointer (&self->api_key,  g_free);
  G_OBJECT_CLASS (llm_ghost_mistral_backend_parent_class)->finalize (object);
}

static void
llm_ghost_mistral_backend_class_init (LlmGhostMistralBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_mistral_backend_finalize;
}

static void
llm_ghost_mistral_backend_init (LlmGhostMistralBackend *self)
{
  self->session     = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
  self->max_tokens  = DEFAULT_MAX_TOKENS;
  self->temperature = DEFAULT_TEMPERATURE;
}

/* ---- construction ------------------------------------------------------- */

static char *
pick (const char *arg, const char *env_name, const char *fallback)
{
  if (arg != NULL && *arg != '\0')
    return g_strdup (arg);
  const char *e = g_getenv (env_name);
  if (e != NULL && *e != '\0')
    return g_strdup (e);
  return g_strdup (fallback);
}

static char *
pick_nullable (const char *arg, const char *env_name)
{
  if (arg != NULL && *arg != '\0')
    return g_strdup (arg);
  const char *e = g_getenv (env_name);
  if (e != NULL && *e != '\0')
    return g_strdup (e);
  return NULL;
}

LlmGhostBackend *
llm_ghost_mistral_backend_new (const char *base_url,
                               const char *model,
                               const char *api_key)
{
  LlmGhostMistralBackend *self = g_object_new (LLM_GHOST_TYPE_MISTRAL_BACKEND, NULL);

  self->base_url = pick (base_url, "LLMGHOST_MISTRAL_BASE_URL", DEFAULT_BASE_URL);
  self->model    = pick (model,    "LLMGHOST_MISTRAL_MODEL",    DEFAULT_MODEL);
  self->api_key  = pick_nullable (api_key, "LLMGHOST_MISTRAL_API_KEY");

  return LLM_GHOST_BACKEND (self);
}
