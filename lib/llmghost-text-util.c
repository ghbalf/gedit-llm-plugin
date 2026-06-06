#define G_LOG_DOMAIN "llmghost-text-util"

#include "llmghost-text-util.h"

#include <string.h>

char *
_llm_ghost_clean_single_line (const char *raw)
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
