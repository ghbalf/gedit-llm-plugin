#define G_LOG_DOMAIN "llmghost-settings"

#include "llmghost-settings-internal.h"

#include <string.h>

char *
_llm_ghost_settings_interpolate (const char *in)
{
  if (in == NULL)
    return g_strdup ("");

  GString *out = g_string_new (NULL);
  const char *p = in;
  while (*p != '\0')
    {
      if (p[0] == '$' && p[1] == '{')
        {
          const char *end = strchr (p + 2, '}');
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              const char *val = g_getenv (name);
              if (val == NULL)
                {
                  g_warning ("environment variable ${%s} is not set; using \"\"", name);
                  val = "";
                }
              g_string_append (out, val);
              g_free (name);
              p = end + 1;
              continue;
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}
