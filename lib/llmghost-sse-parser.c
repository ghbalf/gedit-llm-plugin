#define G_LOG_DOMAIN "llmghost-sse"

#include "llmghost-sse-parser.h"

struct _LlmGhostSseParser
{
  GString  *line;       /* current line, no terminator yet */
  GString  *data;       /* accumulated data: payload for the current event */
  gboolean  have_data;  /* at least one data: line seen in current event */
};

LlmGhostSseParser *
_llm_ghost_sse_parser_new (void)
{
  LlmGhostSseParser *p = g_new0 (LlmGhostSseParser, 1);
  p->line = g_string_new (NULL);
  p->data = g_string_new (NULL);
  return p;
}

void
_llm_ghost_sse_parser_free (LlmGhostSseParser *p)
{
  if (p == NULL)
    return;
  g_string_free (p->line, TRUE);
  g_string_free (p->data, TRUE);
  g_free (p);
}

/* Process one complete line (terminator already stripped by the caller). */
static void
process_line (LlmGhostSseParser *p, GPtrArray *out)
{
  /* Strip a trailing CR (CRLF line endings). */
  if (p->line->len > 0 && p->line->str[p->line->len - 1] == '\r')
    g_string_truncate (p->line, p->line->len - 1);

  const char *s = p->line->str;

  if (*s == '\0')                       /* blank line: dispatch the event */
    {
      if (p->have_data)
        {
          g_ptr_array_add (out, g_strdup (p->data->str));
          g_string_truncate (p->data, 0);
          p->have_data = FALSE;
        }
    }
  else if (g_str_has_prefix (s, "data:"))
    {
      const char *v = s + 5;
      if (*v == ' ')                    /* one optional leading space */
        v++;
      if (p->have_data)
        g_string_append_c (p->data, '\n');
      g_string_append (p->data, v);
      p->have_data = TRUE;
    }
  /* ":" comments and other fields (event:/id:/retry:) are ignored. */

  g_string_truncate (p->line, 0);
}

void
_llm_ghost_sse_parser_feed (LlmGhostSseParser *p, const char *data,
                            gsize len, GPtrArray *out_events)
{
  for (gsize i = 0; i < len; i++)
    {
      char c = data[i];
      if (c == '\n')
        process_line (p, out_events);
      else
        g_string_append_c (p->line, c);
    }
}

void
_llm_ghost_sse_parser_finish (LlmGhostSseParser *p, GPtrArray *out_events)
{
  if (p->line->len > 0)                 /* final line w/o newline */
    process_line (p, out_events);
  if (p->have_data)                     /* event w/o trailing blank line */
    {
      g_ptr_array_add (out_events, g_strdup (p->data->str));
      g_string_truncate (p->data, 0);
      p->have_data = FALSE;
    }
}
