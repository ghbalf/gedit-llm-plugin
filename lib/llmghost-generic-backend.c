#define G_LOG_DOMAIN "llmghost-generic"

#include "llmghost-generic-backend-internal.h"

#include <string.h>

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
