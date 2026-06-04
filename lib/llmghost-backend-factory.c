#define G_LOG_DOMAIN "llmghost-factory"

#include "llmghost-backend-factory.h"

#include "llmghost-ollama-backend.h"
#include "llmghost-openai-backend.h"
#include "llmghost-mistral-backend.h"

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
  return llm_ghost_openai_backend_new (param_string (p, "base_url"),
                                       param_string (p, "model"),
                                       param_string (p, "api_key"),
                                       m);
}

static LlmGhostBackend *
build_mistral (JsonObject *p)
{
  return llm_ghost_mistral_backend_new (param_string (p, "base_url"),
                                        param_string (p, "model"),
                                        param_string (p, "api_key"));
}

LlmGhostBackend *
llm_ghost_backend_new_from_settings (LlmGhostSettings *settings)
{
  const char *which = llm_ghost_settings_get_active_backend (settings);
  JsonObject *p = llm_ghost_settings_get_backend_params (settings, which);

  if (g_strcmp0 (which, "openai") == 0)
    return build_openai (p);
  if (g_strcmp0 (which, "mistral") == 0)
    return build_mistral (p);
  if (g_strcmp0 (which, "ollama") != 0)
    g_warning ("unknown backend \"%s\"; using ollama", which);
  return build_ollama (p);
}
