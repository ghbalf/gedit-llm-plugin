#include "llmghost-openai-backend.h"
#include "llmghost-openai-backend-internal.h"

#include <string.h>

#define CHAT_SYSTEM_PROMPT \
  "You are a code completion engine. Output only the code that belongs " \
  "between the given PREFIX and SUFFIX. No explanations, no markdown " \
  "fences, no repetition of the prefix or suffix."

/* ---- request body builders ---------------------------------------------- */

static char *
finish_builder (JsonBuilder *b)
{
  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, json_builder_get_root (b));
  char *out = json_generator_to_data (gen, NULL);
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
  add_stop_newline (b);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);

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
                                   double      temperature)
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
  add_stop_newline (b);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);

  json_builder_end_object (b);
  g_free (user);
  return finish_builder (b);
}

/* ---- response cleanup --------------------------------------------------- */

char *
_llm_ghost_openai_clean_chat_completion (const char *raw)
{
  if (raw == NULL)
    return g_strdup ("");

  char *trimmed = g_strdup (raw);
  g_strstrip (trimmed);                 /* trims leading + trailing whitespace */

  char *unfenced = trimmed;             /* may be reassigned to a new alloc */
  if (g_str_has_prefix (trimmed, "```"))
    {
      const char *nl = strchr (trimmed, '\n');
      if (nl != NULL)
        {
          const char *inner = nl + 1;
          char *close = g_strrstr (inner, "```");
          unfenced = close != NULL
                       ? g_strndup (inner, (gsize) (close - inner))
                       : g_strdup (inner);
          g_strstrip (unfenced);
        }
    }

  const char *nl2 = strchr (unfenced, '\n');
  char *result = nl2 != NULL
                   ? g_strndup (unfenced, (gsize) (nl2 - unfenced))
                   : g_strdup (unfenced);

  if (unfenced != trimmed)
    g_free (unfenced);
  g_free (trimmed);
  return result;
}

/* ---- response extraction ------------------------------------------------ */

char *
_llm_ghost_openai_extract_completion (JsonNode           *root,
                                      LlmGhostOpenAIMode  mode,
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
  return _llm_ghost_openai_clean_chat_completion (content);
}
