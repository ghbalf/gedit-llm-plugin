#define G_LOG_DOMAIN "llmghost-factory"

#include "llmghost-backend-factory.h"

#include "llmghost-ollama-backend.h"
#include "llmghost-openai-backend.h"
#include "llmghost-mistral-backend.h"
#include "llmghost-generic-backend.h"
#include "llmghost-openai-backend-internal.h"
#include "llmghost-generic-backend-internal.h"

#include <json-glib/json-glib.h>

static const char *
param_string (JsonObject *p, const char *key)
{
  if (p == NULL || !json_object_has_member (p, key))
    return NULL;
  JsonNode *n = json_object_get_member (p, key);
  if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (n);
}

static gint64
param_int (JsonObject *p, const char *key, gint64 fallback)
{
  if (p == NULL || !json_object_has_member (p, key))
    return fallback;
  JsonNode *n = json_object_get_member (p, key);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return fallback;
  GType t = json_node_get_value_type (n);
  if (t == G_TYPE_INT64)  return json_node_get_int (n);
  if (t == G_TYPE_DOUBLE) return (gint64) json_node_get_double (n);
  return fallback;
}

static gboolean
param_bool (JsonObject *p, const char *key, gboolean fallback)
{
  if (p == NULL || !json_object_has_member (p, key))
    return fallback;
  JsonNode *n = json_object_get_member (p, key);
  if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_BOOLEAN)
    return fallback;
  return json_node_get_boolean (n);
}

static LlmGhostBackend *
build_ollama (JsonObject *p)
{
  gint64 port = param_int (p, "port", 0);
  if (port < 0 || port > 65535)
    {
      g_warning ("port %" G_GINT64_FORMAT " is out of range [0, 65535]; using default",
                 port);
      port = 0;   /* the ctor treats 0 as "use the Ollama default" */
    }

  LlmGhostBackend *b = llm_ghost_ollama_backend_new (param_string (p, "host"),
                                                     (guint16) port,
                                                     param_string (p, "model"));
  const char *tokens = param_string (p, "tokens");
  if (tokens != NULL && *tokens != '\0')
    {
      const LlmGhostFimTokens *t = llm_ghost_fim_tokens_lookup_builtin (tokens);
      if (t != NULL)
        llm_ghost_ollama_backend_set_fim_tokens (LLM_GHOST_OLLAMA_BACKEND (b), t);
      else
        g_warning ("unknown FIM token set \"%s\"; using default", tokens);
    }
  return b;
}

static LlmGhostBackend *
build_openai (JsonObject *p)
{
  const char *mode = param_string (p, "mode");
  LlmGhostOpenAIMode m =
    (mode != NULL && g_ascii_strcasecmp (mode, "completions") == 0)
      ? LLM_GHOST_OPENAI_MODE_COMPLETIONS
      : LLM_GHOST_OPENAI_MODE_CHAT;
  LlmGhostBackend *b = llm_ghost_openai_backend_new (param_string (p, "base_url"),
                                                     param_string (p, "model"),
                                                     param_string (p, "api_key"),
                                                     m);
  _llm_ghost_openai_backend_set_stream (LLM_GHOST_OPENAI_BACKEND (b),
                                        param_bool (p, "stream", TRUE));
  return b;
}

static LlmGhostBackend *
build_mistral (JsonObject *p)
{
  return llm_ghost_mistral_backend_new (param_string (p, "base_url"),
                                        param_string (p, "model"),
                                        param_string (p, "api_key"));
}

static JsonObject *
param_object (JsonObject *p, const char *key)
{
  if (p == NULL || !json_object_has_member (p, key))
    return NULL;
  JsonNode *n = json_object_get_member (p, key);
  return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static LlmGhostBackend *
build_generic (JsonObject *p)
{
  LlmGhostBackend *b = llm_ghost_generic_backend_new (param_string (p, "url"),
                                                      param_object (p, "headers"),
                                                      param_string (p, "model"),
                                                      param_object (p, "request_template"),
                                                      param_string (p, "response_path"));
  _llm_ghost_generic_backend_set_streaming (LLM_GHOST_GENERIC_BACKEND (b),
                                            param_bool   (p, "stream", TRUE),
                                            param_string (p, "stream_path"),
                                            param_string (p, "done_marker"),
                                            param_string (p, "stream_field"));
  return b;
}

LlmGhostBackend *
llm_ghost_backend_new_from_settings (LlmGhostSettings *settings)
{
  const char *which = llm_ghost_settings_get_active_backend (settings);
  JsonObject *p = llm_ghost_settings_get_backend_params (settings, which);

  guint max_lines = 8;
  llm_ghost_settings_get_max_lines (settings, &max_lines);
  gboolean single_line = (max_lines == 1);

  LlmGhostBackend *b;
  if (g_strcmp0 (which, "openai") == 0)
    {
      b = build_openai (p);
      _llm_ghost_openai_backend_set_single_line (LLM_GHOST_OPENAI_BACKEND (b),
                                                 single_line);
    }
  else if (g_strcmp0 (which, "mistral") == 0)
    {
      b = build_mistral (p);
      llm_ghost_mistral_backend_set_single_line (LLM_GHOST_MISTRAL_BACKEND (b),
                                                 single_line);
    }
  else if (g_strcmp0 (which, "generic") == 0)
    {
      b = build_generic (p);
      llm_ghost_generic_backend_set_single_line (LLM_GHOST_GENERIC_BACKEND (b),
                                                 single_line);
    }
  else
    {
      if (g_strcmp0 (which, "ollama") != 0)
        g_warning ("unknown backend \"%s\"; using ollama", which);
      b = build_ollama (p);
      llm_ghost_ollama_backend_set_single_line (LLM_GHOST_OLLAMA_BACKEND (b),
                                                single_line);
    }
  return b;
}
