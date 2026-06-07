#pragma once

/* Internal (NOT installed). Pure, stateful SSE framing parser: turns an
 * arbitrarily-chunked byte stream into complete "data:" event payloads.
 * No I/O, no JSON. The [DONE] sentinel is emitted as an ordinary payload —
 * interpreting it is the caller's job. */

#include <glib.h>

G_BEGIN_DECLS

typedef struct _LlmGhostSseParser LlmGhostSseParser;

LlmGhostSseParser *_llm_ghost_sse_parser_new  (void);
void               _llm_ghost_sse_parser_free (LlmGhostSseParser *p);

/* Feed @len bytes from @data. For each COMPLETE event (terminated by a blank
 * line), append its assembled payload (newly-allocated char*) to @out_events.
 * Multiple "data:" lines in one event join with '\n'. event:/id:/retry: and
 * comment (":") lines are ignored. Incomplete trailing bytes are retained.
 * Lines are terminated by '\n' (a trailing '\r' is stripped, so CRLF works);
 * a lone CR is NOT treated as a terminator, which is fine for the '\n'/CRLF
 * streams the OpenAI-compatible backends emit. */
void _llm_ghost_sse_parser_feed   (LlmGhostSseParser *p, const char *data,
                                   gsize len, GPtrArray *out_events);

/* Flush a final event buffered without a trailing blank line (stream EOF).
 * NOTE: the WHATWG SSE spec discards an incomplete event at EOF; we instead
 * flush it, which is safe and useful for the [DONE]-terminated LLM streams. */
void _llm_ghost_sse_parser_finish (LlmGhostSseParser *p, GPtrArray *out_events);

G_END_DECLS
