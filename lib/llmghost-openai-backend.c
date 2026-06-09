#include "llmghost-openai-backend.h"
#include "llmghost-openai-backend-internal.h"

#include <libsoup/soup.h>
#include <string.h>
#include "llmghost-http-util.h"
#include "llmghost-text-util.h"
#include "llmghost-backend-internal.h"

#define CHAT_SYSTEM_PROMPT \
  "You are a code completion engine. Output only the code that belongs " \
  "between the given PREFIX and SUFFIX. No explanations, no markdown " \
  "fences, no repetition of the prefix or suffix."

/* ---- request body builders ---------------------------------------------- */

static char *
finish_builder (JsonBuilder *b)
{
  JsonGenerator *gen  = json_generator_new ();
  JsonNode      *root = json_builder_get_root (b);   /* transfer full */
  json_generator_set_root (gen, root);               /* transfer none */
  char *out = json_generator_to_data (gen, NULL);
  json_node_unref (root);
  g_object_unref (gen);
  g_object_unref (b);
  return out;
}

static void
add_stop_newline (JsonBuilder *b)
{
  json_builder_set_member_name (b, "stop");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "\n");
  json_builder_end_array (b);
}

char *
_llm_ghost_openai_build_completions_body (const char *model,
                                          const char *prefix,
                                          const char *suffix,
                                          guint       max_tokens,
                                          double      temperature,
                                          gboolean    stream,
                                          gboolean    single_line)
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
  if (single_line)
    add_stop_newline (b);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, stream);

  json_builder_end_object (b);
  return finish_builder (b);
}

static void
add_message (JsonBuilder *b, const char *role, const char *content)
{
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, role);
  json_builder_set_member_name (b, "content");
  json_builder_add_string_value (b, content);
  json_builder_end_object (b);
}

char *
_llm_ghost_openai_build_chat_body (const char *model,
                                   const char *prefix,
                                   const char *suffix,
                                   guint       max_tokens,
                                   double      temperature,
                                   gboolean    stream,
                                   gboolean    single_line)
{
  char *user = g_strdup_printf ("<PREFIX>%s</PREFIX>\n<SUFFIX>%s</SUFFIX>",
                                prefix ? prefix : "", suffix ? suffix : "");

  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, model ? model : "");

  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  add_message (b, "system", CHAT_SYSTEM_PROMPT);
  add_message (b, "user", user);
  json_builder_end_array (b);

  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, max_tokens);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, temperature);
  if (single_line)
    add_stop_newline (b);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, stream);

  json_builder_end_object (b);
  g_free (user);
  return finish_builder (b);
}

/* ---- response extraction ------------------------------------------------ */

char *
_llm_ghost_openai_extract_completion (JsonNode           *root,
                                      LlmGhostOpenAIMode  mode,
                                      gboolean            single_line,
                                      GError            **error)
{
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "openai: malformed response");
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
                   "openai: %s", msg ? msg : "(error)");
      return NULL;
    }

  if (!json_object_has_member (obj, "choices"))
    return g_strdup ("");

  JsonArray *choices = json_object_get_array_member (obj, "choices");
  if (choices == NULL || json_array_get_length (choices) == 0)
    return g_strdup ("");

  JsonObject *choice = json_array_get_object_element (choices, 0);

  if (mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS)
    {
      const char *text = json_object_has_member (choice, "text")
                             ? json_object_get_string_member (choice, "text")
                             : "";
      return g_strdup (text ? text : "");
    }

  const char *content = "";
  if (json_object_has_member (choice, "message"))
    {
      JsonObject *m = json_object_get_object_member (choice, "message");
      if (m != NULL && json_object_has_member (m, "content"))
        content = json_object_get_string_member (m, "content");
    }
  return _llm_ghost_clean_text (content, single_line);
}

char *
_llm_ghost_openai_extract_delta (JsonNode           *event,
                                 LlmGhostOpenAIMode  mode,
                                 GError            **error)
{
  if (event == NULL || !JSON_NODE_HOLDS_OBJECT (event))
    return g_strdup ("");

  JsonObject *obj = json_node_get_object (event);

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
        msg = json_node_get_string (en);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "openai: %s", msg ? msg : "(error)");
      return NULL;
    }

  if (!json_object_has_member (obj, "choices"))
    return g_strdup ("");
  JsonNode *cn = json_object_get_member (obj, "choices");
  if (!JSON_NODE_HOLDS_ARRAY (cn))
    return g_strdup ("");
  JsonArray *choices = json_node_get_array (cn);
  if (json_array_get_length (choices) == 0)
    return g_strdup ("");
  JsonNode *ce = json_array_get_element (choices, 0);
  if (!JSON_NODE_HOLDS_OBJECT (ce))
    return g_strdup ("");
  JsonObject *choice = json_node_get_object (ce);

  if (mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS)
    {
      if (json_object_has_member (choice, "text"))
        {
          JsonNode *tn = json_object_get_member (choice, "text");
          if (JSON_NODE_HOLDS_VALUE (tn) &&
              json_node_get_value_type (tn) == G_TYPE_STRING)
            return g_strdup (json_node_get_string (tn));
        }
      return g_strdup ("");
    }

  /* chat: choices[0].delta.content */
  if (json_object_has_member (choice, "delta"))
    {
      JsonNode *dn = json_object_get_member (choice, "delta");
      if (JSON_NODE_HOLDS_OBJECT (dn))
        {
          JsonObject *d = json_node_get_object (dn);
          if (json_object_has_member (d, "content"))
            {
              JsonNode *cont = json_object_get_member (d, "content");
              if (JSON_NODE_HOLDS_VALUE (cont) &&
                  json_node_get_value_type (cont) == G_TYPE_STRING)
                return g_strdup (json_node_get_string (cont));
            }
        }
    }
  return g_strdup ("");
}

/* ---- type --------------------------------------------------------------- */

#define DEFAULT_BASE_URL     "https://api.openai.com/v1"
#define DEFAULT_MAX_TOKENS   64
#define DEFAULT_TEMPERATURE  0.2
#define REQUEST_TIMEOUT_SEC  30

struct _LlmGhostOpenAIBackend
{
  GObject             parent_instance;

  SoupSession        *session;
  char               *base_url;
  char               *model;
  char               *api_key;     /* NULL = no auth */
  LlmGhostOpenAIMode  mode;
  gboolean            stream;
  gboolean            single_line;
  guint               max_tokens;
  double              temperature;
};

static void llm_ghost_openai_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostOpenAIBackend, llm_ghost_openai_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_openai_backend_iface_init))

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
  GTask              *task  = G_TASK (user_data);
  LlmGhostOpenAIMode  mode  = (LlmGhostOpenAIMode) GPOINTER_TO_INT (g_task_get_task_data (task));
  GError             *error = NULL;

  JsonNode *root = _llm_ghost_http_post_json_finish (result, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  LlmGhostOpenAIBackend *self = LLM_GHOST_OPENAI_BACKEND (g_task_get_source_object (task));
  char *out = _llm_ghost_openai_extract_completion (root, mode, self->single_line, &error);
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

typedef struct {
  LlmGhostOpenAIBackend *self;   /* borrowed; outer task holds a ref */
  LlmGhostOpenAIMode     mode;
  gboolean               single_line;
  GString               *acc;
  GError                *evt_error;
} OpenAIStreamCtx;

static void
openai_stream_ctx_free (gpointer data)
{
  OpenAIStreamCtx *c = data;
  g_string_free (c->acc, TRUE);
  g_clear_error (&c->evt_error);
  g_free (c);
}

static void
openai_on_event (const char *payload, gpointer user_data)
{
  OpenAIStreamCtx *ctx = user_data;
  if (ctx->evt_error != NULL)
    return;
  if (g_strcmp0 (payload, "[DONE]") == 0)
    return;

  JsonNode *node = _llm_ghost_http_parse_json (payload);
  if (node == NULL)
    return;   /* skip a malformed/empty line, keep streaming */

  GError *e = NULL;
  char *delta = _llm_ghost_openai_extract_delta (node, ctx->mode, &e);
  json_node_unref (node);
  if (delta == NULL)
    {
      ctx->evt_error = e;
      return;
    }
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
openai_on_stream_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask *outer = user_data;
  OpenAIStreamCtx *ctx = g_task_get_task_data (outer);
  GError *error = NULL;

  if (!_llm_ghost_http_post_json_stream_finish (result, &error))
    {
      g_task_return_error (outer, error);
      g_object_unref (outer);
      return;
    }
  if (ctx->evt_error != NULL)
    {
      g_task_return_error (outer, g_steal_pointer (&ctx->evt_error));
      g_object_unref (outer);
      return;
    }

  char *out = (ctx->mode == LLM_GHOST_OPENAI_MODE_CHAT)
                ? _llm_ghost_clean_text (ctx->acc->str, ctx->single_line)
                : g_strdup (ctx->acc->str);
  g_task_return_pointer (outer, out, g_free);
  g_object_unref (outer);
}

static void
openai_request (LlmGhostBackend     *backend,
                const char          *prefix,
                const char          *suffix,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  LlmGhostOpenAIBackend *self = LLM_GHOST_OPENAI_BACKEND (backend);
  gboolean comp = (self->mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS);
  char *url = join_url (self->base_url, comp ? "completions" : "chat/completions");

  if (self->stream)
    {
      GTask *task = g_task_new (self, cancellable, callback, user_data);
      OpenAIStreamCtx *ctx = g_new0 (OpenAIStreamCtx, 1);
      ctx->self = self;
      ctx->mode = self->mode;
      ctx->single_line = self->single_line;
      ctx->acc  = g_string_new (NULL);
      g_task_set_task_data (task, ctx, openai_stream_ctx_free);

      char *body = comp
        ? _llm_ghost_openai_build_completions_body (self->model, prefix, suffix,
                                                    self->max_tokens, self->temperature, TRUE,
                                                    self->single_line)
        : _llm_ghost_openai_build_chat_body (self->model, prefix, suffix,
                                             self->max_tokens, self->temperature, TRUE,
                                             self->single_line);

      JsonObject *headers = NULL;
      if (self->api_key != NULL && *self->api_key != '\0')
        {
          char *auth = g_strdup_printf ("Bearer %s", self->api_key);
          headers = json_object_new ();
          json_object_set_string_member (headers, "Authorization", auth);
          g_free (auth);
        }

      _llm_ghost_http_post_json_stream_async (self->session, url, headers, body,
                                              openai_on_event, ctx, cancellable,
                                              openai_on_stream_done, task);
      if (headers != NULL)
        json_object_unref (headers);
      g_free (url);
      return;
    }

  /* Non-streaming (opt-out) path: whole-body request as before. */
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, GINT_TO_POINTER (self->mode), NULL);
  char *body = comp
    ? _llm_ghost_openai_build_completions_body (self->model, prefix, suffix,
                                                self->max_tokens, self->temperature, FALSE,
                                                self->single_line)
    : _llm_ghost_openai_build_chat_body (self->model, prefix, suffix,
                                         self->max_tokens, self->temperature, FALSE,
                                         self->single_line);
  _llm_ghost_http_post_json_async (self->session, url, self->api_key, body,
                                   cancellable, on_http_done, task);
  g_free (url);
}

static char *
openai_request_finish (LlmGhostBackend *backend, GAsyncResult *result, GError **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_openai_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = openai_request;
  iface->request_finish = openai_request_finish;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llm_ghost_openai_backend_finalize (GObject *object)
{
  LlmGhostOpenAIBackend *self = LLM_GHOST_OPENAI_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->model,    g_free);
  g_clear_pointer (&self->api_key,  g_free);
  G_OBJECT_CLASS (llm_ghost_openai_backend_parent_class)->finalize (object);
}

static void
llm_ghost_openai_backend_class_init (LlmGhostOpenAIBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_openai_backend_finalize;
}

static void
llm_ghost_openai_backend_init (LlmGhostOpenAIBackend *self)
{
  self->session     = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
  self->max_tokens   = DEFAULT_MAX_TOKENS;
  self->temperature  = DEFAULT_TEMPERATURE;
  self->mode         = LLM_GHOST_OPENAI_MODE_CHAT;
  self->stream       = TRUE;
  self->single_line  = TRUE;
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

static LlmGhostOpenAIMode
resolve_mode (LlmGhostOpenAIMode fallback)
{
  const char *e = g_getenv ("LLMGHOST_OPENAI_MODE");
  if (e == NULL || *e == '\0')
    return fallback;
  if (g_ascii_strcasecmp (e, "completions") == 0)
    return LLM_GHOST_OPENAI_MODE_COMPLETIONS;
  if (g_ascii_strcasecmp (e, "chat") == 0)
    return LLM_GHOST_OPENAI_MODE_CHAT;
  g_printerr ("llmghost: unknown LLMGHOST_OPENAI_MODE '%s'; using default\n", e);
  return fallback;
}

LlmGhostBackend *
llm_ghost_openai_backend_new (const char         *base_url,
                              const char         *model,
                              const char         *api_key,
                              LlmGhostOpenAIMode  mode)
{
  LlmGhostOpenAIBackend *self = g_object_new (LLM_GHOST_TYPE_OPENAI_BACKEND, NULL);

  self->base_url = pick (base_url, "LLMGHOST_OPENAI_BASE_URL", DEFAULT_BASE_URL);
  self->model    = pick (model,    "LLMGHOST_OPENAI_MODEL",    "");
  self->api_key  = pick_nullable (api_key, "LLMGHOST_OPENAI_API_KEY");
  self->mode     = resolve_mode (mode);

  return LLM_GHOST_BACKEND (self);
}

void
_llm_ghost_openai_backend_set_stream (LlmGhostOpenAIBackend *self, gboolean stream)
{
  g_return_if_fail (LLM_GHOST_IS_OPENAI_BACKEND (self));
  self->stream = stream;
}

void
_llm_ghost_openai_backend_set_single_line (LlmGhostOpenAIBackend *self,
                                           gboolean               single_line)
{
  g_return_if_fail (LLM_GHOST_IS_OPENAI_BACKEND (self));
  self->single_line = single_line;
}
