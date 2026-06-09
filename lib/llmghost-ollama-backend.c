#include "llmghost-ollama-backend.h"
#include "llmghost-ollama-backend-internal.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "llmghost-http-util.h"

#define DEFAULT_HOST         "spark-2448"
#define DEFAULT_PORT         11434
#define DEFAULT_MODEL        "qwen3-coder-next:latest"
#define DEFAULT_NUM_PREDICT  64
#define DEFAULT_TEMPERATURE  0.2
#define REQUEST_TIMEOUT_SEC  30

struct _LlmGhostOllamaBackend
{
  GObject             parent_instance;

  SoupSession        *session;
  char               *host;
  guint16             port;
  char               *model;
  LlmGhostFimTokens  *fim_tokens;   /* never NULL after init */

  guint               num_predict;
  double              temperature;
  gboolean            single_line;
};

static void llm_ghost_ollama_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostOllamaBackend, llm_ghost_ollama_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_ollama_backend_iface_init))

/* ---- request body builder ----------------------------------------------- */

char *
_llm_ghost_ollama_build_request_body (const char              *model,
                                      const LlmGhostFimTokens *tokens,
                                      const char              *prefix,
                                      const char              *suffix,
                                      guint                    num_predict,
                                      double                   temperature,
                                      gboolean                 single_line)
{
  /* Bypass Ollama's prompt-template layer with raw=true and inject the
   * configured family's FIM sentinels directly. Works on models whose
   * Modelfile registers no `insert` template (the common case). */
  char *fim = g_strconcat (tokens->prefix_tok, prefix ? prefix : "",
                           tokens->suffix_tok, suffix ? suffix : "",
                           tokens->middle_tok, NULL);

  JsonBuilder *b = json_builder_new ();

  json_builder_begin_object (b);

  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, model);

  json_builder_set_member_name (b, "prompt");
  json_builder_add_string_value (b, fim);

  json_builder_set_member_name (b, "raw");
  json_builder_add_boolean_value (b, TRUE);

  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);

  json_builder_set_member_name (b, "options");
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "num_predict");
  json_builder_add_int_value (b, num_predict);

  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, temperature);

  json_builder_set_member_name (b, "stop");
  json_builder_begin_array (b);
  /* Newline forces single-line completion (phase 2). The remaining stops
   * are family-specific sentinel tokens that should never leak into the
   * response. */
  if (single_line)
    json_builder_add_string_value (b, "\n");
  for (gsize i = 0; tokens->stop_tokens != NULL && tokens->stop_tokens[i] != NULL; i++)
    json_builder_add_string_value (b, tokens->stop_tokens[i]);
  json_builder_end_array (b);

  json_builder_end_object (b); /* options */
  json_builder_end_object (b); /* root */

  JsonGenerator *gen  = json_generator_new ();
  JsonNode      *root = json_builder_get_root (b);   /* transfer full */
  json_generator_set_root (gen, root);               /* transfer none */
  char *body = json_generator_to_data (gen, NULL);

  json_node_unref (root);
  g_object_unref (gen);
  g_object_unref (b);
  g_free (fim);

  return body;
}

/* ---- async response handler --------------------------------------------- */

static void
on_ollama_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask    *task  = G_TASK (user_data);
  GError   *error = NULL;
  JsonNode *root  = _llm_ghost_http_post_json_finish (result, &error);

  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  JsonObject *obj = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  if (obj == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ollama: malformed JSON response");
      json_node_unref (root);
      g_object_unref (task);
      return;
    }

  if (json_object_has_member (obj, "error"))
    {
      const char *err = json_object_get_string_member (obj, "error");
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ollama: %s", err ? err : "(no message)");
      json_node_unref (root);
      g_object_unref (task);
      return;
    }

  const char *response = json_object_has_member (obj, "response")
                             ? json_object_get_string_member (obj, "response")
                             : "";
  char *out = g_strdup (response ? response : "");
  json_node_unref (root);

  g_task_return_pointer (task, out, g_free);
  g_object_unref (task);
}

/* ---- LlmGhostBackend interface ------------------------------------------ */

static void
ollama_request (LlmGhostBackend     *backend,
                const char          *prefix,
                const char          *suffix,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  LlmGhostOllamaBackend *self = LLM_GHOST_OLLAMA_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  char *url = g_strdup_printf ("http://%s:%u/api/generate",
                               self->host, (unsigned) self->port);
  char *body = _llm_ghost_ollama_build_request_body (self->model, self->fim_tokens,
                                                     prefix, suffix,
                                                     self->num_predict, self->temperature,
                                                     self->single_line);

  _llm_ghost_http_post_json_async (self->session, url, NULL, body,
                                   cancellable, on_ollama_response, task);
  g_free (url);
}

static char *
ollama_request_finish (LlmGhostBackend  *backend,
                       GAsyncResult     *result,
                       GError          **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_ollama_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = ollama_request;
  iface->request_finish = ollama_request_finish;
}

/* ---- Public API --------------------------------------------------------- */

void
llm_ghost_ollama_backend_set_fim_tokens (LlmGhostOllamaBackend   *self,
                                         const LlmGhostFimTokens *tokens)
{
  g_return_if_fail (LLM_GHOST_IS_OLLAMA_BACKEND (self));

  const LlmGhostFimTokens *src = tokens != NULL
                                   ? tokens
                                   : llm_ghost_fim_tokens_qwen ();
  g_clear_pointer (&self->fim_tokens, llm_ghost_fim_tokens_free);
  self->fim_tokens = llm_ghost_fim_tokens_copy (src);
}

void
llm_ghost_ollama_backend_set_single_line (LlmGhostOllamaBackend *self,
                                          gboolean               single_line)
{
  g_return_if_fail (LLM_GHOST_IS_OLLAMA_BACKEND (self));
  self->single_line = single_line;
}

/* ---- GObject lifecycle --------------------------------------------------- */

static void
llm_ghost_ollama_backend_finalize (GObject *object)
{
  LlmGhostOllamaBackend *self = LLM_GHOST_OLLAMA_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->host,       g_free);
  g_clear_pointer (&self->model,      g_free);
  g_clear_pointer (&self->fim_tokens, llm_ghost_fim_tokens_free);
  G_OBJECT_CLASS (llm_ghost_ollama_backend_parent_class)->finalize (object);
}

static void
llm_ghost_ollama_backend_class_init (LlmGhostOllamaBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_ollama_backend_finalize;
}

static void
llm_ghost_ollama_backend_init (LlmGhostOllamaBackend *self)
{
  self->session = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
  self->num_predict  = DEFAULT_NUM_PREDICT;
  self->temperature  = DEFAULT_TEMPERATURE;
  self->single_line  = TRUE;
  self->fim_tokens   = llm_ghost_fim_tokens_copy (llm_ghost_fim_tokens_qwen ());
}

LlmGhostBackend *
llm_ghost_ollama_backend_new (const char *host,
                              guint16     port,
                              const char *model)
{
  LlmGhostOllamaBackend *self = g_object_new (LLM_GHOST_TYPE_OLLAMA_BACKEND, NULL);

  self->host  = g_strdup ((host  && *host)  ? host  : DEFAULT_HOST);
  self->port  = port ? port : DEFAULT_PORT;
  self->model = g_strdup ((model && *model) ? model : DEFAULT_MODEL);

  return LLM_GHOST_BACKEND (self);
}
