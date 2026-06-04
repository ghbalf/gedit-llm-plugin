#include "llmghost-mistral-backend.h"
#include "llmghost-mistral-backend-internal.h"

#include <string.h>

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
