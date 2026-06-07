# SSE Streaming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream completion tokens into the ghost overlay as they arrive (instead of waiting for the whole HTTP body), for the OpenAI-compatible and generic/template backends.

**Architecture:** A pure SSE framing parser turns chunked bytes into `data:` payloads; a new streaming `http-util` transport reads the response as a `GInputStream` and drives the parser; a `partial-data` signal on the `LlmGhostBackend` interface carries the accumulated text; the OpenAI and generic backends extract per-event deltas, accumulate, and emit it; the controller renders each emission. `request_finish` still returns the full completion (or a `GError`, which clears the ghost).

**Tech Stack:** C (gnu11), GLib/GObject/GIO, libsoup-3, json-glib, meson/ninja, GLib `g_test` (+ Xvfb for the gui suite).

**Reference spec:** `docs/superpowers/specs/2026-06-07-sse-streaming-design.md`

## Conventions (read before starting)

- Every `.c` starts with `#define G_LOG_DOMAIN "..."` before includes.
- **Internal headers** (`*-internal.h`, `llmghost-http-util.h`, `llmghost-sse-parser.h`, `llmghost-backend-internal.h`) go into `llmghost_sources` **only via their `.c`** — never add a header to `llmghost_headers` (installed set).
- Known false positives — do NOT "fix": clang-tidy `bugprone-sizeof-expression` on `g_clear_*`/`g_clear_pointer`/`g_clear_object` lines; clangd "unused include" on the `llmghost.h` umbrella.
- Build + test commands (run from repo root):
  - Configure once: `meson setup build` (if `build/` absent).
  - Build: `ninja -C build`
  - One suite: `meson test -C build --suite unit` / `--suite gui`
  - One test: `meson test -C build <name>` (e.g. `sse-parser`)
  - A failing build counts as a failing step — fix before moving on.

## File Structure

**New files**
- `lib/llmghost-sse-parser.h` / `.c` — pure SSE framing parser (internal).
- `lib/llmghost-backend-internal.h` — `partial-data` emit helper (internal).
- `tests/test-sse-parser.c` — parser unit tests.
- `tests/test-backend-signal.c` — signal-emission unit test.

**Modified files**
- `lib/llmghost-http-util.h` / `.c` — streaming transport + `_llm_ghost_http_parse_json`.
- `lib/llmghost-backend.h` / `.c` — signal name macro + registration.
- `lib/llmghost-openai-backend.c` + `-internal.h` — stream field/setter, body `stream` flag, delta extractor, streaming flow.
- `lib/llmghost-generic-backend.c` + `-internal.h` — stream config/setter, body `stream` override, delta extractor, streaming flow.
- `lib/llmghost-backend-factory.c` — `param_bool` + wire the new config to the setters.
- `lib/llmghost-controller.c` — connect `partial-data` → overlay.
- `tests/mock-backend.{c,h}` — `mock_backend_emit_partial` helper.
- `tests/test-http-util.c`, `tests/test-openai-body.c`, `tests/test-generic-body.c`, `tests/test-controller.c` — new tests.
- `lib/meson.build`, `tests/meson.build` — register new sources/tests.
- `NOTES.md` — mark the feature landed.

---

### Task 1: Pure SSE framing parser

**Files:**
- Create: `lib/llmghost-sse-parser.h`
- Create: `lib/llmghost-sse-parser.c`
- Create: `tests/test-sse-parser.c`
- Modify: `lib/meson.build` (add source), `tests/meson.build` (add test)

- [ ] **Step 1: Create the header**

Create `lib/llmghost-sse-parser.h`:

```c
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
 * comment (":") lines are ignored. Incomplete trailing bytes are retained. */
void _llm_ghost_sse_parser_feed   (LlmGhostSseParser *p, const char *data,
                                   gsize len, GPtrArray *out_events);

/* Flush a final event buffered without a trailing blank line (stream EOF). */
void _llm_ghost_sse_parser_finish (LlmGhostSseParser *p, GPtrArray *out_events);

G_END_DECLS
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test-sse-parser.c`:

```c
#include <glib.h>
#include <string.h>
#include "llmghost-sse-parser.h"

static GPtrArray *
feed_all (const char *const *chunks, gboolean finish)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  LlmGhostSseParser *p = _llm_ghost_sse_parser_new ();
  for (int i = 0; chunks[i] != NULL; i++)
    _llm_ghost_sse_parser_feed (p, chunks[i], strlen (chunks[i]), out);
  if (finish)
    _llm_ghost_sse_parser_finish (p, out);
  _llm_ghost_sse_parser_free (p);
  return out;
}

static void
test_single_event (void)
{
  const char *chunks[] = { "data: hello\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "hello");
  g_ptr_array_unref (out);
}

static void
test_done_passthrough (void)
{
  const char *chunks[] = { "data: [DONE]\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "[DONE]");
  g_ptr_array_unref (out);
}

static void
test_split_across_chunks (void)
{
  const char *chunks[] = { "data: hel", "lo\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "hello");
  g_ptr_array_unref (out);
}

static void
test_multiple_events_one_chunk (void)
{
  const char *chunks[] = { "data: a\n\ndata: b\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "a");
  g_assert_cmpstr (g_ptr_array_index (out, 1), ==, "b");
  g_ptr_array_unref (out);
}

static void
test_crlf (void)
{
  const char *chunks[] = { "data: x\r\n\r\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "x");
  g_ptr_array_unref (out);
}

static void
test_comment_and_event_lines_ignored (void)
{
  const char *chunks[] = { ": ping\n\nevent: msg\ndata: y\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "y");
  g_ptr_array_unref (out);
}

static void
test_multi_data_concat (void)
{
  const char *chunks[] = { "data: a\ndata: b\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "a\nb");
  g_ptr_array_unref (out);
}

static void
test_finish_flushes_unterminated (void)
{
  const char *chunks[] = { "data: z\n", NULL };
  GPtrArray *out = feed_all (chunks, TRUE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "z");
  g_ptr_array_unref (out);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sse/single-event",        test_single_event);
  g_test_add_func ("/sse/done-passthrough",     test_done_passthrough);
  g_test_add_func ("/sse/split-across-chunks",  test_split_across_chunks);
  g_test_add_func ("/sse/multiple-events",      test_multiple_events_one_chunk);
  g_test_add_func ("/sse/crlf",                 test_crlf);
  g_test_add_func ("/sse/comment-event-ignored", test_comment_and_event_lines_ignored);
  g_test_add_func ("/sse/multi-data-concat",    test_multi_data_concat);
  g_test_add_func ("/sse/finish-flushes",       test_finish_flushes_unterminated);
  return g_test_run ();
}
```

- [ ] **Step 3: Register source + test in meson, verify it fails to build**

In `lib/meson.build`, add to `llmghost_sources` (after `'llmghost-secret-store.c',`):

```
  'llmghost-sse-parser.c',
```

In `tests/meson.build`, add after the `test_secret_store` block:

```
test_sse_parser = executable(
  'test-sse-parser',
  'test-sse-parser.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('sse-parser', test_sse_parser, suite: 'unit')
```

Run: `ninja -C build`
Expected: FAIL — `undefined reference to '_llm_ghost_sse_parser_new'` (no `.c` yet).

- [ ] **Step 4: Implement the parser**

Create `lib/llmghost-sse-parser.c`:

```c
#define G_LOG_DOMAIN "llmghost-sse"

#include "llmghost-sse-parser.h"
#include <string.h>

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
```

- [ ] **Step 5: Build and run the tests**

Run: `ninja -C build && meson test -C build sse-parser -v`
Expected: PASS, 8/8.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-sse-parser.c lib/llmghost-sse-parser.h tests/test-sse-parser.c lib/meson.build tests/meson.build
git commit -m "feat(sse): pure SSE framing parser"
```

---

### Task 2: `partial-data` signal on the backend interface

**Files:**
- Modify: `lib/llmghost-backend.h`
- Modify: `lib/llmghost-backend.c`
- Create: `lib/llmghost-backend-internal.h`
- Create: `tests/test-backend-signal.c`
- Modify: `tests/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-backend-signal.c`:

```c
#include <glib.h>
#include "llmghost-backend.h"
#include "llmghost-backend-internal.h"
#include "mock-backend.h"

typedef struct { char *last; guint count; } Seen;

static void
on_partial (LlmGhostBackend *b, const char *text, gpointer user_data)
{
  (void) b;
  Seen *s = user_data;
  g_free (s->last);
  s->last = g_strdup (text);
  s->count++;
}

static void
test_emit_partial_data (void)
{
  LlmGhostBackend *b = mock_backend_new ();
  Seen seen = { 0 };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &seen);

  _llm_ghost_backend_emit_partial_data (b, "hel");
  _llm_ghost_backend_emit_partial_data (b, "hello");

  g_assert_cmpuint (seen.count, ==, 2);
  g_assert_cmpstr (seen.last, ==, "hello");

  g_free (seen.last);
  g_object_unref (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/backend/emit-partial-data", test_emit_partial_data);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test in meson and verify it fails to build**

In `tests/meson.build`, add after the `test_sse_parser` block:

```
test_backend_signal = executable(
  'test-backend-signal',
  ['test-backend-signal.c', mock_backend_sources],
  dependencies: [llmghost_dep],
  install: false,
)
test('backend-signal', test_backend_signal, suite: 'unit')
```

Run: `ninja -C build`
Expected: FAIL — `LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA` undeclared / `undefined reference to '_llm_ghost_backend_emit_partial_data'`.

- [ ] **Step 3: Add the public signal-name macro**

In `lib/llmghost-backend.h`, add immediately after the `G_DECLARE_INTERFACE (...)` line (line 8):

```c

/* Emitted by streaming backends as completion text accumulates. Signature:
 *   void (*) (LlmGhostBackend *self, const char *accumulated_text)
 * request_finish() still returns the full completion; non-streaming backends
 * never emit this. */
#define LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA "partial-data"
```

- [ ] **Step 4: Create the internal emit-helper header**

Create `lib/llmghost-backend-internal.h`:

```c
#pragma once

/* Internal (NOT installed). Helper for streaming backends to emit the
 * interface's "partial-data" signal. */

#include "llmghost-backend.h"

G_BEGIN_DECLS

/* Emit "partial-data" on @self carrying the accumulated completion text. */
void _llm_ghost_backend_emit_partial_data (LlmGhostBackend *self,
                                           const char      *accumulated);

G_END_DECLS
```

- [ ] **Step 5: Register the signal + implement the emit helper**

Replace the body of `lib/llmghost-backend.c` with:

```c
#include "llmghost-backend.h"
#include "llmghost-backend-internal.h"

G_DEFINE_INTERFACE (LlmGhostBackend, llm_ghost_backend, G_TYPE_OBJECT)

static void
llm_ghost_backend_default_init (LlmGhostBackendInterface *iface)
{
  (void) iface;

  g_signal_new (LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                LLM_GHOST_TYPE_BACKEND,
                G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_STRING);
}

void
_llm_ghost_backend_emit_partial_data (LlmGhostBackend *self,
                                      const char      *accumulated)
{
  g_return_if_fail (LLM_GHOST_IS_BACKEND (self));
  g_signal_emit_by_name (self, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA, accumulated);
}

void
llm_ghost_backend_request (LlmGhostBackend     *self,
                           const char          *prefix,
                           const char          *suffix,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  g_return_if_fail (LLM_GHOST_IS_BACKEND (self));
  LLM_GHOST_BACKEND_GET_IFACE (self)->request (self, prefix, suffix,
                                               cancellable, callback, user_data);
}

char *
llm_ghost_backend_request_finish (LlmGhostBackend  *self,
                                  GAsyncResult     *result,
                                  GError          **error)
{
  g_return_val_if_fail (LLM_GHOST_IS_BACKEND (self), NULL);
  return LLM_GHOST_BACKEND_GET_IFACE (self)->request_finish (self, result, error);
}
```

- [ ] **Step 6: Build and run the test**

Run: `ninja -C build && meson test -C build backend-signal -v`
Expected: PASS, 1/1.

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-backend.c lib/llmghost-backend.h lib/llmghost-backend-internal.h tests/test-backend-signal.c tests/meson.build
git commit -m "feat(backend): partial-data signal on the backend interface"
```

---

### Task 3: Streaming HTTP transport + JSON parse helper

**Files:**
- Modify: `lib/llmghost-http-util.h`
- Modify: `lib/llmghost-http-util.c`
- Modify: `tests/test-http-util.c`

- [ ] **Step 1: Write the failing tests**

In `tests/test-http-util.c`, add the streaming server routes. In `server_cb`, add these branches **before** the final `else /* /malformed */` (i.e. after the `/bad` branch, change `else` to `else if`):

```c
  else if (g_strcmp0 (path, "/sse") == 0)
    {
      const char *resp =
        "data: a\n\n"
        "data: b\n\n"
        "data: [DONE]\n\n";
      soup_server_message_set_status (m, 200, NULL);
      soup_server_message_set_response (m, "text/event-stream",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
  else if (g_strcmp0 (path, "/sse-bad") == 0)
    {
      const char *resp = "{\"error\":\"nope\"}";
      soup_server_message_set_status (m, 500, NULL);
      soup_server_message_set_response (m, "application/json",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
```

Add a streaming driver + tests at the end of the file, before `main`:

```c
/* ---- streaming driver -------------------------------------------------- */

typedef struct {
  GMainLoop *loop;
  GPtrArray *events;   /* char*, owned */
  gboolean   ok;
  GError    *error;
} StreamWait;

static void
on_stream_event (const char *payload, gpointer user_data)
{
  StreamWait *w = user_data;
  g_ptr_array_add (w->events, g_strdup (payload));
}

static void
on_stream_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  StreamWait *w = user_data;
  w->ok = _llm_ghost_http_post_json_stream_finish (result, &w->error);
  g_main_loop_quit (w->loop);
}

static StreamWait *
stream_post (Srv *s, const char *path)
{
  SoupSession *session = soup_session_new ();
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  StreamWait *w = g_new0 (StreamWait, 1);
  w->loop = loop;
  w->events = g_ptr_array_new_with_free_func (g_free);
  char *url = g_strconcat (s->base, path + 1, NULL);

  _llm_ghost_http_post_json_stream_async (session, url, NULL, g_strdup ("{}"),
                                          on_stream_event, w, NULL,
                                          on_stream_done, w);
  g_main_loop_run (loop);

  g_free (url);
  g_main_loop_unref (loop);
  g_object_unref (session);
  return w;
}

static void
stream_wait_free (StreamWait *w)
{
  g_ptr_array_unref (w->events);
  g_clear_error (&w->error);
  g_free (w);
}

static void
test_stream_delivers_events (void)
{
  Srv *s = srv_new ();
  StreamWait *w = stream_post (s, "/sse");
  g_assert_true (w->ok);
  g_assert_no_error (w->error);
  g_assert_cmpuint (w->events->len, ==, 3);
  g_assert_cmpstr (g_ptr_array_index (w->events, 0), ==, "a");
  g_assert_cmpstr (g_ptr_array_index (w->events, 1), ==, "b");
  g_assert_cmpstr (g_ptr_array_index (w->events, 2), ==, "[DONE]");
  stream_wait_free (w);
  srv_free (s);
}

static void
test_stream_http_error (void)
{
  Srv *s = srv_new ();
  StreamWait *w = stream_post (s, "/sse-bad");
  g_assert_false (w->ok);
  g_assert_error (w->error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (w->error->message, -1, "500"));
  stream_wait_free (w);
  srv_free (s);
}

static void
test_parse_json_helper (void)
{
  JsonNode *n = _llm_ghost_http_parse_json ("{\"k\":42}");
  g_assert_nonnull (n);
  g_assert_cmpint (json_object_get_int_member (json_node_get_object (n), "k"), ==, 42);
  json_node_unref (n);

  g_assert_null (_llm_ghost_http_parse_json (""));
  g_assert_null (_llm_ghost_http_parse_json ("not json {"));
}
```

Register them in `main` (add before `return g_test_run ();`):

```c
  g_test_add_func ("/http-util/stream-events",   test_stream_delivers_events);
  g_test_add_func ("/http-util/stream-http-error", test_stream_http_error);
  g_test_add_func ("/http-util/parse-json",      test_parse_json_helper);
```

- [ ] **Step 2: Verify the test fails to build**

Run: `ninja -C build`
Expected: FAIL — `undefined reference to '_llm_ghost_http_post_json_stream_async'`, `_llm_ghost_http_parse_json`.

- [ ] **Step 3: Declare the new API in the header**

In `lib/llmghost-http-util.h`, add before `G_END_DECLS`:

```c

/* Parse a JSON document @text into a newly-allocated root node (caller owns
 * via json_node_unref), or NULL if @text is empty or not valid JSON. */
JsonNode * _llm_ghost_http_parse_json (const char *text);

/* Called for each complete SSE "data:" payload, in order, on the main context. */
typedef void (*LlmGhostSseEventFn) (const char *payload, gpointer user_data);

/* POST @json_body to @url and stream the text/event-stream response: each
 * complete event payload is delivered to @on_event(@event_data). @headers
 * (string->string, nullable) are applied as for the non-streaming call; supply
 * auth here. Takes ownership of @json_body. @session, @cancellable, @headers,
 * @event_data are borrowed (read synchronously before return / during reads).
 * Finish with _llm_ghost_http_post_json_stream_finish(). */
void       _llm_ghost_http_post_json_stream_async  (SoupSession         *session,
                                                    const char          *url,
                                                    JsonObject          *headers,
                                                    char                *json_body,
                                                    LlmGhostSseEventFn   on_event,
                                                    gpointer             event_data,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

/* TRUE when the stream completed (EOF after a 2xx). FALSE + @error on transport
 * failure, non-2xx HTTP (message carries the status + body snippet), or cancel. */
gboolean   _llm_ghost_http_post_json_stream_finish (GAsyncResult        *result,
                                                    GError             **error);
```

- [ ] **Step 4: Refactor message construction + implement the transport**

In `lib/llmghost-http-util.c`, **replace** `_llm_ghost_http_post_json_headers_async` (lines 63–121) with a shared message builder + the slimmed async entry. First, add this static helper just above `_llm_ghost_http_post_json_headers_async`:

```c
/* Build a POST SoupMessage for @url with @headers + JSON @json_body. Consumes
 * @json_body in all paths. Returns NULL + @error (G_IO_ERROR_INVALID_ARGUMENT)
 * on an invalid URL. */
static SoupMessage *
make_post_message (const char *url, JsonObject *headers,
                   char *json_body, GError **error)
{
  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  if (msg == NULL)
    {
      g_free (json_body);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "invalid URL: %s", url ? url : "(null)");
      return NULL;
    }

  const char *content_type = "application/json";
  SoupMessageHeaders *h = soup_message_get_request_headers (msg);
  if (headers != NULL)
    {
      JsonObjectIter iter;
      const char *name;
      JsonNode *val;
      json_object_iter_init (&iter, headers);
      while (json_object_iter_next (&iter, &name, &val))
        {
          if (!JSON_NODE_HOLDS_VALUE (val) ||
              json_node_get_value_type (val) != G_TYPE_STRING)
            {
              g_warning ("header \"%s\" is not a string; skipping", name);
              continue;
            }
          if (g_ascii_strcasecmp (name, "Content-Type") == 0)
            {
              content_type = json_node_get_string (val);
              continue;
            }
          soup_message_headers_append (h, name, json_node_get_string (val));
        }
    }

  GBytes *bytes = g_bytes_new_take (json_body, strlen (json_body));
  soup_message_set_request_body_from_bytes (msg, content_type, bytes);
  g_bytes_unref (bytes);
  return msg;
}
```

Then replace the body of `_llm_ghost_http_post_json_headers_async` with:

```c
void
_llm_ghost_http_post_json_headers_async (SoupSession         *session,
                                         const char          *url,
                                         JsonObject          *headers,
                                         char                *json_body,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GTask *task = g_task_new (session, cancellable, callback, user_data);
  GError *error = NULL;
  SoupMessage *msg = make_post_message (url, headers, json_body, &error);
  if (msg == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  /* Keep the SoupMessage alive until the handler reads its status. */
  g_task_set_task_data (task, msg, g_object_unref);
  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_soup_response, task);
}
```

Now add the parse helper + streaming transport at the end of the file (before nothing — just append):

```c
JsonNode *
_llm_ghost_http_parse_json (const char *text)
{
  if (text == NULL || *text == '\0')
    return NULL;
  JsonParser *parser = json_parser_new ();
  JsonNode *copy = NULL;
  if (json_parser_load_from_data (parser, text, -1, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL)
        copy = json_node_copy (root);
    }
  g_object_unref (parser);
  return copy;
}

/* ---- streaming transport ----------------------------------------------- */

typedef struct {
  SoupMessage        *msg;          /* owned */
  GInputStream       *stream;       /* owned (set after send) */
  LlmGhostSseParser  *parser;       /* owned */
  GPtrArray          *events;       /* scratch, owned */
  LlmGhostSseEventFn  on_event;     /* borrowed */
  gpointer            event_data;   /* borrowed */
  guint               status;
  gboolean            is_error;     /* non-2xx: accumulate body for the message */
  GString            *errbuf;       /* owned when is_error */
  char                buf[4096];
} StreamCtx;

static void
stream_ctx_free (gpointer data)
{
  StreamCtx *c = data;
  g_clear_object (&c->msg);
  g_clear_object (&c->stream);
  if (c->parser != NULL)
    _llm_ghost_sse_parser_free (c->parser);
  if (c->events != NULL)
    g_ptr_array_unref (c->events);
  if (c->errbuf != NULL)
    g_string_free (c->errbuf, TRUE);
  g_free (c);
}

static void
flush_events (StreamCtx *ctx)
{
  for (guint i = 0; i < ctx->events->len; i++)
    ctx->on_event (g_ptr_array_index (ctx->events, i), ctx->event_data);
  g_ptr_array_set_size (ctx->events, 0);   /* frees elements (free func set) */
}

static void read_chunk (GTask *task);

static void
on_read (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GTask *task = user_data;
  StreamCtx *ctx = g_task_get_task_data (task);
  GError *error = NULL;

  gssize n = g_input_stream_read_finish (ctx->stream, res, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  if (n == 0)   /* EOF */
    {
      if (ctx->is_error)
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "HTTP %u: %.*s", ctx->status,
                                 (int) MIN (ctx->errbuf->len, 256u),
                                 ctx->errbuf->str);
      else
        {
          _llm_ghost_sse_parser_finish (ctx->parser, ctx->events);
          flush_events (ctx);
          g_task_return_boolean (task, TRUE);
        }
      g_object_unref (task);
      return;
    }

  if (ctx->is_error)
    g_string_append_len (ctx->errbuf, ctx->buf, n);
  else
    {
      _llm_ghost_sse_parser_feed (ctx->parser, ctx->buf, (gsize) n, ctx->events);
      flush_events (ctx);
    }

  read_chunk (task);
}

static void
read_chunk (GTask *task)
{
  StreamCtx *ctx = g_task_get_task_data (task);
  g_input_stream_read_async (ctx->stream, ctx->buf, sizeof ctx->buf,
                             G_PRIORITY_DEFAULT, g_task_get_cancellable (task),
                             on_read, task);
}

static void
on_send_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GTask *task = user_data;
  StreamCtx *ctx = g_task_get_task_data (task);
  GError *error = NULL;

  GInputStream *stream =
    soup_session_send_finish (SOUP_SESSION (source), res, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  ctx->stream = stream;   /* owned */
  ctx->status = soup_message_get_status (ctx->msg);
  if (ctx->status < 200 || ctx->status >= 300)
    {
      ctx->is_error = TRUE;
      ctx->errbuf = g_string_new (NULL);
    }
  read_chunk (task);
}

void
_llm_ghost_http_post_json_stream_async (SoupSession         *session,
                                        const char          *url,
                                        JsonObject          *headers,
                                        char                *json_body,
                                        LlmGhostSseEventFn   on_event,
                                        gpointer             event_data,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  GTask *task = g_task_new (session, cancellable, callback, user_data);
  GError *error = NULL;
  SoupMessage *msg = make_post_message (url, headers, json_body, &error);
  if (msg == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  StreamCtx *ctx = g_new0 (StreamCtx, 1);
  ctx->msg        = msg;   /* owned */
  ctx->parser     = _llm_ghost_sse_parser_new ();
  ctx->events     = g_ptr_array_new_with_free_func (g_free);
  ctx->on_event   = on_event;
  ctx->event_data = event_data;
  g_task_set_task_data (task, ctx, stream_ctx_free);

  soup_session_send_async (session, msg, G_PRIORITY_DEFAULT,
                           cancellable, on_send_ready, task);
}

gboolean
_llm_ghost_http_post_json_stream_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
```

Add the include for the parser near the top of `lib/llmghost-http-util.c` (after `#include "llmghost-http-util.h"`):

```c
#include "llmghost-sse-parser.h"
```

- [ ] **Step 5: Build and run**

Run: `ninja -C build && meson test -C build http-util -v`
Expected: PASS — the 6 existing http-util tests **plus** the 3 new ones (9 total). The existing tests confirm the `make_post_message` refactor preserved behavior.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-http-util.c lib/llmghost-http-util.h tests/test-http-util.c
git commit -m "feat(http): streaming SSE transport + JSON parse helper"
```

---

### Task 4: OpenAI streaming

**Files:**
- Modify: `lib/llmghost-openai-backend-internal.h`
- Modify: `lib/llmghost-openai-backend.c`
- Modify: `tests/test-openai-body.c`

- [ ] **Step 1: Write failing pure-unit tests (builder flag + delta extractor)**

In `tests/test-openai-body.c` you will (a) update existing calls to the two builders to pass the new trailing `stream` arg, and (b) add delta-extractor tests. First add these test functions (place near the other tests):

```c
static void
test_chat_body_stream_flag (void)
{
  char *on  = _llm_ghost_openai_build_chat_body ("m", "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_openai_build_chat_body ("m", "p", "s", 64, 0.2, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stream\":true"));
  g_assert_nonnull (g_strstr_len (off, -1, "\"stream\":false"));
  g_free (on);
  g_free (off);
}

static void
test_delta_chat (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"content\":\"He\"}}]}");
  GError *e = NULL;
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, &e);
  g_assert_no_error (e);
  g_assert_cmpstr (d, ==, "He");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_completions (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"text\":\"xy\"}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_COMPLETIONS, NULL);
  g_assert_cmpstr (d, ==, "xy");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_role_only_is_empty (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, NULL);
  g_assert_cmpstr (d, ==, "");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_finish_chunk_is_empty (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, NULL);
  g_assert_cmpstr (d, ==, "");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_error_member (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"error\":{\"message\":\"boom\"}}");
  GError *e = NULL;
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, &e);
  g_assert_null (d);
  g_assert_error (e, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (e->message, -1, "boom"));
  g_clear_error (&e);
  json_node_unref (n);
}
```

Add `#include "llmghost-http-util.h"` to the top of `tests/test-openai-body.c` (for `_llm_ghost_http_parse_json`). Update the **existing** builder call sites in that file to append the new arg, e.g. a call `_llm_ghost_openai_build_chat_body ("m","p","s",64,0.2)` becomes `_llm_ghost_openai_build_chat_body ("m","p","s",64,0.2, FALSE)` (do the same for every `_build_completions_body`/`_build_chat_body` call). Register the new tests in that file's `main`:

```c
  g_test_add_func ("/openai/chat-body-stream-flag", test_chat_body_stream_flag);
  g_test_add_func ("/openai/delta-chat",            test_delta_chat);
  g_test_add_func ("/openai/delta-completions",     test_delta_completions);
  g_test_add_func ("/openai/delta-role-only",       test_delta_role_only_is_empty);
  g_test_add_func ("/openai/delta-finish-chunk",    test_delta_finish_chunk_is_empty);
  g_test_add_func ("/openai/delta-error-member",    test_delta_error_member);
```

- [ ] **Step 2: Verify it fails to build**

Run: `ninja -C build`
Expected: FAIL — builders take the wrong number of args / `_llm_ghost_openai_extract_delta` undeclared.

- [ ] **Step 3: Update the internal header**

In `lib/llmghost-openai-backend-internal.h`, change the two builder prototypes to add a trailing `gboolean stream`, add the delta-extractor prototype, and add the stream setter. Replace lines 12–29 with:

```c
char *_llm_ghost_openai_build_completions_body (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature,
                                                gboolean    stream);

char *_llm_ghost_openai_build_chat_body        (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature,
                                                gboolean    stream);

/* Pull the completion text from a parsed response @root. For CHAT, cleans
 * via _llm_ghost_clean_single_line. Returns "" for no/empty choices; NULL +
 * @error when the body carries an API error object. */
char *_llm_ghost_openai_extract_completion     (JsonNode           *root,
                                                LlmGhostOpenAIMode  mode,
                                                GError            **error);

/* Extract the incremental delta text from one streaming event @event. Returns
 * "" when the event carries no content (role-only opener, finish chunk).
 * chat -> choices[0].delta.content; completions -> choices[0].text. NULL +
 * @error when @event carries an API error object. Newly-allocated. */
char *_llm_ghost_openai_extract_delta          (JsonNode           *event,
                                                LlmGhostOpenAIMode  mode,
                                                GError            **error);

/* Override the default streaming behavior (default: TRUE = stream when able). */
void  _llm_ghost_openai_backend_set_stream     (LlmGhostOpenAIBackend *self,
                                                gboolean               stream);
```

- [ ] **Step 4: Implement in the backend**

In `lib/llmghost-openai-backend.c`:

(a) Add includes near the top (after the existing includes):

```c
#include "llmghost-backend-internal.h"   /* _llm_ghost_backend_emit_partial_data */
```
(`llmghost-http-util.h` and `llmghost-text-util.h` are already included.)

(b) Add the `stream` parameter to both body builders. In `_llm_ghost_openai_build_completions_body`, change the signature to end with `, double temperature, guint max_tokens... , gboolean stream)` — concretely, append `, gboolean stream` to the parameter list, and replace the two lines:

```c
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);
```
with:
```c
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, stream);
```
Do the **same** in `_llm_ghost_openai_build_chat_body` (append `, gboolean stream` to its signature and use `stream` in `add_boolean_value`).

(c) Add the delta extractor after `_llm_ghost_openai_extract_completion`:

```c
char *
_llm_ghost_openai_extract_delta (JsonNode           *event,
                                 LlmGhostOpenAIMode  mode,
                                 GError            **error)
{
  if (event == NULL || !JSON_NODE_HOLDS_OBJECT (event))
    return g_strdup ("");

  JsonObject *obj = json_node_get_object (event);

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
        msg = json_node_get_string (en);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "openai: %s", msg ? msg : "(error)");
      return NULL;
    }

  if (!json_object_has_member (obj, "choices"))
    return g_strdup ("");
  JsonNode *cn = json_object_get_member (obj, "choices");
  if (!JSON_NODE_HOLDS_ARRAY (cn))
    return g_strdup ("");
  JsonArray *choices = json_node_get_array (cn);
  if (json_array_get_length (choices) == 0)
    return g_strdup ("");
  JsonNode *ce = json_array_get_element (choices, 0);
  if (!JSON_NODE_HOLDS_OBJECT (ce))
    return g_strdup ("");
  JsonObject *choice = json_node_get_object (ce);

  if (mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS)
    {
      if (json_object_has_member (choice, "text"))
        {
          JsonNode *tn = json_object_get_member (choice, "text");
          if (JSON_NODE_HOLDS_VALUE (tn) &&
              json_node_get_value_type (tn) == G_TYPE_STRING)
            return g_strdup (json_node_get_string (tn));
        }
      return g_strdup ("");
    }

  /* chat: choices[0].delta.content */
  if (json_object_has_member (choice, "delta"))
    {
      JsonNode *dn = json_object_get_member (choice, "delta");
      if (JSON_NODE_HOLDS_OBJECT (dn))
        {
          JsonObject *d = json_node_get_object (dn);
          if (json_object_has_member (d, "content"))
            {
              JsonNode *cont = json_object_get_member (d, "content");
              if (JSON_NODE_HOLDS_VALUE (cont) &&
                  json_node_get_value_type (cont) == G_TYPE_STRING)
                return g_strdup (json_node_get_string (cont));
            }
        }
    }
  return g_strdup ("");
}
```

(d) Add a `gboolean stream;` field to `struct _LlmGhostOpenAIBackend` (after `LlmGhostOpenAIMode mode;`), default it in init, and add the setter. In `llm_ghost_openai_backend_init`, add:

```c
  self->stream      = TRUE;
```
After `llm_ghost_openai_backend_new` (or anywhere among the functions), add:

```c
void
_llm_ghost_openai_backend_set_stream (LlmGhostOpenAIBackend *self, gboolean stream)
{
  g_return_if_fail (LLM_GHOST_IS_OPENAI_BACKEND (self));
  self->stream = stream;
}
```

(e) Update the **existing** non-stream call sites in `openai_request` to pass `FALSE` (they will only run when `self->stream` is false). Then add the streaming branch. Replace the whole `openai_request` function with:

```c
typedef struct {
  LlmGhostOpenAIBackend *self;   /* borrowed; outer task holds a ref */
  LlmGhostOpenAIMode     mode;
  GString               *acc;
  GError                *evt_error;
} OpenAIStreamCtx;

static void
openai_stream_ctx_free (gpointer data)
{
  OpenAIStreamCtx *c = data;
  g_string_free (c->acc, TRUE);
  g_clear_error (&c->evt_error);
  g_free (c);
}

static void
openai_on_event (const char *payload, gpointer user_data)
{
  OpenAIStreamCtx *ctx = user_data;
  if (ctx->evt_error != NULL)
    return;
  if (g_strcmp0 (payload, "[DONE]") == 0)
    return;

  JsonNode *node = _llm_ghost_http_parse_json (payload);
  if (node == NULL)
    return;   /* skip a malformed/empty line, keep streaming */

  GError *e = NULL;
  char *delta = _llm_ghost_openai_extract_delta (node, ctx->mode, &e);
  json_node_unref (node);
  if (delta == NULL)
    {
      ctx->evt_error = e;
      return;
    }
  if (*delta != '\0')
    {
      g_string_append (ctx->acc, delta);
      char *clean = _llm_ghost_clean_single_line (ctx->acc->str);
      _llm_ghost_backend_emit_partial_data (LLM_GHOST_BACKEND (ctx->self), clean);
      g_free (clean);
    }
  g_free (delta);
}

static void
openai_on_stream_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask *outer = user_data;
  OpenAIStreamCtx *ctx = g_task_get_task_data (outer);
  GError *error = NULL;

  if (!_llm_ghost_http_post_json_stream_finish (result, &error))
    {
      g_task_return_error (outer, error);
      g_object_unref (outer);
      return;
    }
  if (ctx->evt_error != NULL)
    {
      g_task_return_error (outer, g_steal_pointer (&ctx->evt_error));
      g_object_unref (outer);
      return;
    }

  char *out = (ctx->mode == LLM_GHOST_OPENAI_MODE_CHAT)
                ? _llm_ghost_clean_single_line (ctx->acc->str)
                : g_strdup (ctx->acc->str);
  g_task_return_pointer (outer, out, g_free);
  g_object_unref (outer);
}

static void
openai_request (LlmGhostBackend     *backend,
                const char          *prefix,
                const char          *suffix,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  LlmGhostOpenAIBackend *self = LLM_GHOST_OPENAI_BACKEND (backend);
  gboolean comp = (self->mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS);
  char *url = join_url (self->base_url, comp ? "completions" : "chat/completions");

  if (self->stream)
    {
      GTask *task = g_task_new (self, cancellable, callback, user_data);
      OpenAIStreamCtx *ctx = g_new0 (OpenAIStreamCtx, 1);
      ctx->self = self;
      ctx->mode = self->mode;
      ctx->acc  = g_string_new (NULL);
      g_task_set_task_data (task, ctx, openai_stream_ctx_free);

      char *body = comp
        ? _llm_ghost_openai_build_completions_body (self->model, prefix, suffix,
                                                    self->max_tokens, self->temperature, TRUE)
        : _llm_ghost_openai_build_chat_body (self->model, prefix, suffix,
                                             self->max_tokens, self->temperature, TRUE);

      JsonObject *headers = NULL;
      if (self->api_key != NULL && *self->api_key != '\0')
        {
          char *auth = g_strdup_printf ("Bearer %s", self->api_key);
          headers = json_object_new ();
          json_object_set_string_member (headers, "Authorization", auth);
          g_free (auth);
        }

      _llm_ghost_http_post_json_stream_async (self->session, url, headers, body,
                                              openai_on_event, ctx, cancellable,
                                              openai_on_stream_done, task);
      if (headers != NULL)
        json_object_unref (headers);
      g_free (url);
      return;
    }

  /* Non-streaming (opt-out) path: whole-body request as before. */
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, GINT_TO_POINTER (self->mode), NULL);
  char *body = comp
    ? _llm_ghost_openai_build_completions_body (self->model, prefix, suffix,
                                                self->max_tokens, self->temperature, FALSE)
    : _llm_ghost_openai_build_chat_body (self->model, prefix, suffix,
                                         self->max_tokens, self->temperature, FALSE);
  _llm_ghost_http_post_json_async (self->session, url, self->api_key, body,
                                   cancellable, on_http_done, task);
  g_free (url);
}
```

(The `OpenAIStreamCtx` struct + its helpers must appear **before** `openai_request`. Place the `typedef`/`openai_stream_ctx_free`/`openai_on_event`/`openai_on_stream_done` block above `openai_request` in the `request flow` section, after `on_http_done`.)

- [ ] **Step 5: Build and run the pure tests**

Run: `ninja -C build && meson test -C build openai-body -v`
Expected: PASS — existing builder tests (with updated args) + the 6 new tests.

- [ ] **Step 6: Add a loopback streaming integration test**

Create the streaming integration test as part of `tests/test-openai-body.c` is awkward (it has no server); instead add it to a small new test file `tests/test-openai-stream.c`:

```c
#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include "llmghost-openai-backend.h"
#include "llmghost-openai-backend-internal.h"
#include "llmghost-backend.h"

static void
server_cb (SoupServer *server, SoupServerMessage *m, const char *path,
           GHashTable *query, gpointer user_data)
{
  (void) server; (void) path; (void) query; (void) user_data;
  const char *resp =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
    "data: [DONE]\n\n";
  soup_server_message_set_status (m, 200, NULL);
  soup_server_message_set_response (m, "text/event-stream",
                                    SOUP_MEMORY_COPY, resp, strlen (resp));
}

typedef struct {
  GMainLoop *loop;
  char      *final;
  GError    *error;
  guint      partial_count;
  char      *last_partial;
} Run;

static void
on_partial (LlmGhostBackend *b, const char *text, gpointer user_data)
{
  (void) b;
  Run *r = user_data;
  r->partial_count++;
  g_free (r->last_partial);
  r->last_partial = g_strdup (text);
}

static void
on_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  Run *r = user_data;
  r->final = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                               result, &r->error);
  g_main_loop_quit (r->loop);
}

static void
test_openai_streams_chat (void)
{
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, server_cb, NULL, NULL);
  GError *err = NULL;
  g_assert_true (soup_server_listen_local (server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
  g_assert_no_error (err);
  GSList *uris = soup_server_get_uris (server);
  char *base = g_uri_to_string (uris->data);   /* trailing slash */
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  LlmGhostBackend *b = llm_ghost_openai_backend_new (base, "m", NULL,
                                                     LLM_GHOST_OPENAI_MODE_CHAT);
  _llm_ghost_openai_backend_set_stream (LLM_GHOST_OPENAI_BACKEND (b), TRUE);

  Run r = { .loop = g_main_loop_new (NULL, FALSE) };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &r);
  llm_ghost_backend_request (b, "pre", "suf", NULL, on_done, &r);
  g_main_loop_run (r.loop);

  g_assert_no_error (r.error);
  g_assert_cmpstr (r.final, ==, "Hello world");
  g_assert_cmpuint (r.partial_count, ==, 2);
  g_assert_cmpstr (r.last_partial, ==, "Hello world");

  g_free (r.final);
  g_free (r.last_partial);
  g_main_loop_unref (r.loop);
  g_free (base);
  g_object_unref (b);
  g_object_unref (server);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/openai-stream/chat", test_openai_streams_chat);
  return g_test_run ();
}
```

Register it in `tests/meson.build` (after the openai-body block):

```
test_openai_stream = executable(
  'test-openai-stream',
  'test-openai-stream.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('openai-stream', test_openai_stream, suite: 'unit')
```

Run: `ninja -C build && meson test -C build openai-stream -v`
Expected: PASS, 1/1 (final text "Hello world", 2 partials).

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-openai-backend.c lib/llmghost-openai-backend-internal.h tests/test-openai-body.c tests/test-openai-stream.c tests/meson.build
git commit -m "feat(openai): SSE streaming with partial-data emission"
```

---

### Task 5: Generic backend streaming

**Files:**
- Modify: `lib/llmghost-generic-backend-internal.h`
- Modify: `lib/llmghost-generic-backend.c`
- Modify: `tests/test-generic-body.c`

- [ ] **Step 1: Write failing pure-unit tests (delta extractor + stream-field body override)**

In `tests/test-generic-body.c`, add `#include "llmghost-http-util.h"` at the top (for `_llm_ghost_http_parse_json`). Add tests:

```c
static void
test_extract_delta_present (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}");
  char *d = _llm_ghost_generic_extract_delta (n, "choices.0.delta.content");
  g_assert_cmpstr (d, ==, "hi");
  g_free (d);
  json_node_unref (n);
}

static void
test_extract_delta_missing_is_empty (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
  char *d = _llm_ghost_generic_extract_delta (n, "choices.0.delta.content");
  g_assert_cmpstr (d, ==, "");   /* lenient: missing path -> "" */
  g_free (d);
  json_node_unref (n);
}

static void
test_build_body_stream_override (void)
{
  JsonObject *tmpl = json_object_new ();
  json_object_set_string_member (tmpl, "prompt", "{{prefix}}");

  char *on = _llm_ghost_generic_build_body_with_stream (tmpl, "p", "s", NULL,
                                                        "stream", TRUE);
  g_assert_nonnull (g_strstr_len (on, -1, "\"stream\":true"));
  g_free (on);

  char *off = _llm_ghost_generic_build_body_with_stream (tmpl, "p", "s", NULL,
                                                         "stream", FALSE);
  g_assert_nonnull (g_strstr_len (off, -1, "\"stream\":false"));
  g_free (off);

  /* Empty stream_field leaves the template untouched (no stream member). */
  char *none = _llm_ghost_generic_build_body_with_stream (tmpl, "p", "s", NULL,
                                                          "", TRUE);
  g_assert_null (g_strstr_len (none, -1, "stream"));
  g_free (none);

  json_object_unref (tmpl);
}
```

Register in that file's `main`:

```c
  g_test_add_func ("/generic/extract-delta-present",  test_extract_delta_present);
  g_test_add_func ("/generic/extract-delta-missing",  test_extract_delta_missing_is_empty);
  g_test_add_func ("/generic/build-body-stream",      test_build_body_stream_override);
```

- [ ] **Step 2: Verify it fails to build**

Run: `ninja -C build`
Expected: FAIL — `_llm_ghost_generic_extract_delta` / `_llm_ghost_generic_build_body_with_stream` undeclared.

- [ ] **Step 3: Update the internal header**

In `lib/llmghost-generic-backend-internal.h`, add before `G_END_DECLS`:

```c

/* Like _llm_ghost_generic_build_body but also sets the top-level @stream_field
 * member of the body object to @stream_value before serializing. If
 * @stream_field is NULL or "", the template is left untouched. */
char *_llm_ghost_generic_build_body_with_stream (JsonObject *template,
                                                 const char *prefix,
                                                 const char *suffix,
                                                 const char *model,
                                                 const char *stream_field,
                                                 gboolean    stream_value);

/* Like _llm_ghost_generic_extract but returns "" (never an error) when @path
 * is absent / not a string in @event — for per-event SSE deltas. */
char *_llm_ghost_generic_extract_delta (JsonNode   *event,
                                        const char *path);

/* Configure streaming. @stream gates it; streaming is active only when
 * @stream is TRUE and @stream_path is non-empty. @done_marker (NULL/"" ->
 * "[DONE]") is the event payload to skip. @stream_field (NULL/"" -> "stream";
 * "" via explicit empty disables body mutation) names the body member set to
 * @stream's wire value. Copies the strings. */
void  _llm_ghost_generic_backend_set_streaming (LlmGhostGenericBackend *self,
                                                gboolean    stream,
                                                const char *stream_path,
                                                const char *done_marker,
                                                const char *stream_field);
```

Add the include for the type at the top of the internal header (it currently does not include the public header). Add after the existing includes:

```c
#include "llmghost-generic-backend.h"
```

- [ ] **Step 4: Implement in the backend**

In `lib/llmghost-generic-backend.c`:

(a) Add includes (after the existing ones at the top):

```c
#include "llmghost-backend-internal.h"   /* _llm_ghost_backend_emit_partial_data */
```
(`llmghost-http-util.h` and `llmghost-text-util.h` are already included.)

(b) Refactor `_llm_ghost_generic_build_body` to delegate, and add the override variant. Replace the existing `_llm_ghost_generic_build_body` function (lines 81–101) with:

```c
char *
_llm_ghost_generic_build_body_with_stream (JsonObject *template,
                                           const char *prefix,
                                           const char *suffix,
                                           const char *model,
                                           const char *stream_field,
                                           gboolean    stream_value)
{
  /* Deep-copy so the stored template is never mutated. */
  JsonNode *wrap = json_node_alloc ();
  json_node_init_object (wrap, template);   /* refs template */
  JsonNode *copy = json_node_copy (wrap);   /* deep copy */
  json_node_unref (wrap);

  substitute_node (copy, prefix, suffix, model);

  if (stream_field != NULL && *stream_field != '\0')
    {
      JsonObject *obj = json_node_get_object (copy);
      json_object_set_boolean_member (obj, stream_field, stream_value);
    }

  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, copy);
  char *out = json_generator_to_data (gen, NULL);
  g_object_unref (gen);
  json_node_unref (copy);
  return out;
}

char *
_llm_ghost_generic_build_body (JsonObject *template,
                               const char *prefix,
                               const char *suffix,
                               const char *model)
{
  return _llm_ghost_generic_build_body_with_stream (template, prefix, suffix,
                                                    model, NULL, FALSE);
}
```

(c) Add the lenient extractor after `_llm_ghost_generic_extract`:

```c
char *
_llm_ghost_generic_extract_delta (JsonNode *event, const char *path)
{
  char *s = _llm_ghost_generic_extract (event, path, NULL);
  return s != NULL ? s : g_strdup ("");
}
```

(d) Add streaming fields to `struct _LlmGhostGenericBackend` (after `char *response_path;`):

```c
  gboolean  stream;          /* gate (default TRUE) */
  char     *stream_path;     /* dotted path to per-event delta, or NULL */
  char     *done_marker;     /* sentinel payload to skip (default "[DONE]") */
  char     *stream_field;    /* body member to set to stream value, or NULL */
```

Default them in `llm_ghost_generic_backend_init` (after `soup_session_set_timeout (...)`):

```c
  self->stream       = TRUE;
  self->done_marker  = g_strdup ("[DONE]");
  self->stream_field = g_strdup ("stream");
```

Free them in `llm_ghost_generic_backend_finalize` (alongside the other `g_clear_pointer` calls):

```c
  g_clear_pointer (&self->stream_path,  g_free);
  g_clear_pointer (&self->done_marker,  g_free);
  g_clear_pointer (&self->stream_field, g_free);
```

(e) Add the setter (place after `llm_ghost_generic_backend_new`):

```c
void
_llm_ghost_generic_backend_set_streaming (LlmGhostGenericBackend *self,
                                          gboolean    stream,
                                          const char *stream_path,
                                          const char *done_marker,
                                          const char *stream_field)
{
  g_return_if_fail (LLM_GHOST_IS_GENERIC_BACKEND (self));
  self->stream = stream;

  g_clear_pointer (&self->stream_path, g_free);
  self->stream_path = (stream_path != NULL && *stream_path != '\0')
                        ? g_strdup (stream_path) : NULL;

  if (done_marker != NULL && *done_marker != '\0')
    {
      g_clear_pointer (&self->done_marker, g_free);
      self->done_marker = g_strdup (done_marker);
    }

  /* NULL -> keep default "stream"; explicit "" -> disable body mutation. */
  if (stream_field != NULL)
    {
      g_clear_pointer (&self->stream_field, g_free);
      self->stream_field = g_strdup (stream_field);
    }
}
```

(f) Add the streaming request flow + branch. Add this block before `generic_request` (after `on_http_done`):

```c
typedef struct {
  LlmGhostGenericBackend *self;   /* borrowed; outer task holds a ref */
  GString                *acc;
  const char             *stream_path;   /* borrowed from self */
  const char             *done_marker;   /* borrowed from self */
} GenericStreamCtx;

static void
generic_stream_ctx_free (gpointer data)
{
  GenericStreamCtx *c = data;
  g_string_free (c->acc, TRUE);
  g_free (c);
}

static void
generic_on_event (const char *payload, gpointer user_data)
{
  GenericStreamCtx *ctx = user_data;
  if (g_strcmp0 (payload, ctx->done_marker) == 0)
    return;

  JsonNode *node = _llm_ghost_http_parse_json (payload);
  if (node == NULL)
    return;

  char *delta = _llm_ghost_generic_extract_delta (node, ctx->stream_path);
  json_node_unref (node);
  if (*delta != '\0')
    {
      g_string_append (ctx->acc, delta);
      char *clean = _llm_ghost_clean_single_line (ctx->acc->str);
      _llm_ghost_backend_emit_partial_data (LLM_GHOST_BACKEND (ctx->self), clean);
      g_free (clean);
    }
  g_free (delta);
}

static void
generic_on_stream_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask *outer = user_data;
  GenericStreamCtx *ctx = g_task_get_task_data (outer);
  GError *error = NULL;

  if (!_llm_ghost_http_post_json_stream_finish (result, &error))
    {
      g_task_return_error (outer, error);
      g_object_unref (outer);
      return;
    }

  char *out = _llm_ghost_clean_single_line (ctx->acc->str);
  g_task_return_pointer (outer, out, g_free);
  g_object_unref (outer);
}
```

Then, in `generic_request`, after the existing config-validation block (after the `if (self->request_template == NULL || ...)` guard returns), insert the streaming branch **before** the existing whole-body call:

```c
  if (self->stream && self->stream_path != NULL && *self->stream_path != '\0')
    {
      GenericStreamCtx *ctx = g_new0 (GenericStreamCtx, 1);
      ctx->self        = self;
      ctx->acc         = g_string_new (NULL);
      ctx->stream_path = self->stream_path;
      ctx->done_marker = self->done_marker;
      g_task_set_task_data (task, ctx, generic_stream_ctx_free);

      char *body = _llm_ghost_generic_build_body_with_stream (
        self->request_template, prefix, suffix, self->model,
        self->stream_field, TRUE);

      _llm_ghost_http_post_json_stream_async (self->session, self->url,
                                              self->headers, body,
                                              generic_on_event, ctx, cancellable,
                                              generic_on_stream_done, task);
      return;
    }
```

(The existing `char *body = _llm_ghost_generic_build_body (...)` + `_llm_ghost_http_post_json_headers_async (...)` lines remain as the non-streaming path below this branch.)

- [ ] **Step 5: Build and run the pure tests**

Run: `ninja -C build && meson test -C build generic-body -v`
Expected: PASS — existing tests + the 3 new ones.

- [ ] **Step 6: Add a loopback streaming integration test**

Create `tests/test-generic-stream.c`:

```c
#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include "llmghost-generic-backend.h"
#include "llmghost-generic-backend-internal.h"
#include "llmghost-backend.h"

static void
server_cb (SoupServer *server, SoupServerMessage *m, const char *path,
           GHashTable *query, gpointer user_data)
{
  (void) server; (void) path; (void) query; (void) user_data;
  const char *resp =
    "data: {\"choices\":[{\"delta\":{\"content\":\"foo\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"bar\"}}]}\n\n"
    "data: [DONE]\n\n";
  soup_server_message_set_status (m, 200, NULL);
  soup_server_message_set_response (m, "text/event-stream",
                                    SOUP_MEMORY_COPY, resp, strlen (resp));
}

typedef struct { GMainLoop *loop; char *final; GError *error; guint partials; } Run;

static void
on_partial (LlmGhostBackend *b, const char *t, gpointer ud)
{ (void) b; (void) t; ((Run *) ud)->partials++; }

static void
on_done (GObject *src, GAsyncResult *res, gpointer ud)
{
  Run *r = ud;
  r->final = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (src), res, &r->error);
  g_main_loop_quit (r->loop);
}

static void
test_generic_streams (void)
{
  SoupServer *server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, NULL, server_cb, NULL, NULL);
  GError *err = NULL;
  g_assert_true (soup_server_listen_local (server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
  g_assert_no_error (err);
  GSList *uris = soup_server_get_uris (server);
  char *base = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  JsonObject *tmpl = json_object_new ();
  json_object_set_string_member (tmpl, "prompt", "{{prefix}}");
  LlmGhostBackend *b = llm_ghost_generic_backend_new (base, NULL, NULL, tmpl,
                                                      "choices.0.message.content");
  _llm_ghost_generic_backend_set_streaming (LLM_GHOST_GENERIC_BACKEND (b), TRUE,
                                            "choices.0.delta.content", "[DONE]", "stream");

  Run r = { .loop = g_main_loop_new (NULL, FALSE) };
  g_signal_connect (b, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                    G_CALLBACK (on_partial), &r);
  llm_ghost_backend_request (b, "pre", "suf", NULL, on_done, &r);
  g_main_loop_run (r.loop);

  g_assert_no_error (r.error);
  g_assert_cmpstr (r.final, ==, "foobar");
  g_assert_cmpuint (r.partials, ==, 2);

  g_free (r.final);
  g_main_loop_unref (r.loop);
  json_object_unref (tmpl);
  g_free (base);
  g_object_unref (b);
  g_object_unref (server);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/generic-stream/basic", test_generic_streams);
  return g_test_run ();
}
```

Register in `tests/meson.build` (after the generic-body block):

```
test_generic_stream = executable(
  'test-generic-stream',
  'test-generic-stream.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('generic-stream', test_generic_stream, suite: 'unit')
```

Run: `ninja -C build && meson test -C build generic-stream -v`
Expected: PASS, 1/1 (final "foobar", 2 partials).

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-generic-backend.c lib/llmghost-generic-backend-internal.h tests/test-generic-body.c tests/test-generic-stream.c tests/meson.build
git commit -m "feat(generic): config-driven SSE streaming"
```

---

### Task 6: Factory wiring (settings → streaming config)

**Files:**
- Modify: `lib/llmghost-backend-factory.c`

**Note on testing:** the factory is a thin JSON→setter mapping; there is no existing factory test harness and the streaming fields are private. Its behavior (default-on, opt-out, generic stream config) is exercised end-to-end by the integration tests in Tasks 4–5 (which call the same setters) and guarded by a clean build + the full suite. No new test file is added here.

- [ ] **Step 1: Add a bool param reader**

In `lib/llmghost-backend-factory.c`, add after `param_int` (line 35):

```c
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
```

- [ ] **Step 2: Add the internal-header includes**

After the existing backend includes (line 8), add:

```c
#include "llmghost-openai-backend-internal.h"
#include "llmghost-generic-backend-internal.h"
```

- [ ] **Step 3: Wire OpenAI streaming opt-out**

Replace `build_openai` (lines 63–75) with:

```c
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
```

- [ ] **Step 4: Wire generic streaming config**

Replace `build_generic` (lines 94–102) with:

```c
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
```

- [ ] **Step 5: Build and run the full unit suite**

Run: `ninja -C build && meson test -C build --suite unit`
Expected: PASS — all unit tests, including the streaming ones.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-backend-factory.c
git commit -m "feat(factory): wire stream config to openai/generic backends"
```

---

### Task 7: Controller renders partial-data

**Files:**
- Modify: `tests/mock-backend.h`
- Modify: `tests/mock-backend.c`
- Modify: `lib/llmghost-controller.c`
- Modify: `tests/test-controller.c`

- [ ] **Step 1: Add a partial-emit helper to the mock backend**

In `tests/mock-backend.h`, add after `mock_backend_complete_pending` (line 18):

```c
void             mock_backend_emit_partial    (MockBackend *self, const char *text);
```

In `tests/mock-backend.c`, add the include at the top (after `#include "mock-backend.h"`):

```c
#include "llmghost-backend-internal.h"
```
and add the function (after `mock_backend_complete_pending`):

```c
void
mock_backend_emit_partial (MockBackend *self, const char *text)
{
  _llm_ghost_backend_emit_partial_data (LLM_GHOST_BACKEND (self), text);
}
```

- [ ] **Step 2: Write the failing GUI tests**

In `tests/test-controller.c`, add a helper to read overlay text (after `ghost_visible`, line 94):

```c
static char *
ghost_text (Fixture *f)
{
  LlmGhostOverlay *o = find_ghost_overlay (f);
  if (o == NULL)
    return g_strdup ("");
  return g_strdup (gtk_label_get_text (GTK_LABEL (o)));
}
```

Add two tests (place before `main`):

```c
static void
test_partial_renders_incrementally (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);   /* request now in-flight (parked in mock) */

  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "He");
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));
  char *t1 = ghost_text (f);
  g_assert_cmpstr (t1, ==, "He");
  g_free (t1);

  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "Hello");
  pump (SETTLE_MS);
  char *t2 = ghost_text (f);
  g_assert_cmpstr (t2, ==, "Hello");
  g_free (t2);

  fixture_free (f);
}

static void
test_partial_gated_when_idle (void)
{
  Fixture *f = fixture_new ();
  /* No request in flight (cancellable == NULL): a stray partial must not show. */
  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "ghost");
  pump (SETTLE_MS);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}
```

Register them in `main`:

```c
  g_test_add_func ("/controller/partial-incremental", test_partial_renders_incrementally);
  g_test_add_func ("/controller/partial-gated-idle",  test_partial_gated_when_idle);
```

- [ ] **Step 3: Verify the tests fail**

Run: `ninja -C build && meson test -C build controller -v`
Expected: FAIL — `/controller/partial-incremental` fails (ghost not shown / wrong text), because the controller doesn't connect the signal yet.

- [ ] **Step 4: Connect the signal in the controller**

In `lib/llmghost-controller.c`:

(a) Add a handler-id field to `struct _LlmGhostController` (after `gulong h_view_destroy;`):

```c
  gulong               h_backend_partial;
```

(b) Add the include for the signal name + (the macro is in the public backend header, already included transitively via controller.h → backend.h; no new include needed). Add the handler function (place near `on_completion_ready`):

```c
static void
on_partial_data (LlmGhostBackend *backend, const char *text, gpointer user_data)
{
  (void) backend;
  LlmGhostController *self = user_data;

  /* Gate: only render while a request is in flight, so a late emission from a
   * cancelled request can't bleed into a new context. */
  if (self->cancellable == NULL)
    return;
  if (self->view == NULL || text == NULL || *text == '\0')
    return;
  if (!cursor_safe_for_ghost (self->view))
    return;

  g_clear_pointer (&self->current_ghost, g_free);
  self->current_ghost = g_strdup (text);
  show_ghost_at_cursor (self);
}
```

(c) Connect it in `llm_ghost_controller_new`, right after `self->backend = g_object_ref (backend);`:

```c
  self->h_backend_partial =
    g_signal_connect (self->backend, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                      G_CALLBACK (on_partial_data), self);
```

(d) Disconnect it in `llm_ghost_controller_dispose`, **before** `g_clear_object (&self->backend);`:

```c
  if (self->backend != NULL && self->h_backend_partial != 0)
    {
      g_signal_handler_disconnect (self->backend, self->h_backend_partial);
      self->h_backend_partial = 0;
    }
```

(e) Add a forward declaration with the other forward decls (after `on_completion_ready`):

```c
static void     on_partial_data           (LlmGhostBackend *backend, const char *text, gpointer user_data);
```

- [ ] **Step 5: Build and run the GUI suite**

Run: `ninja -C build && meson test -C build --suite gui -v`
Expected: PASS — all controller tests including the 2 new ones.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-controller.c tests/mock-backend.c tests/mock-backend.h tests/test-controller.c
git commit -m "feat(controller): render partial-data into the ghost overlay"
```

---

### Task 8: Documentation

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Mark the feature landed**

Open `NOTES.md`, find the roadmap/prerequisite section that mentions SSE streaming and the `partial-data` signal. Add a "SSE streaming (landed)" subsection describing: the `partial-data` signal on `LlmGhostBackend` (carries accumulated text); OpenAI streams by default with a `"stream": false` opt-out; the generic backend streams when `stream_path` is set (config keys `stream`, `stream_path`, `done_marker`, `stream_field`); `request_finish` still returns the full completion and a mid-stream failure clears the ghost; ghost rendering is single-line (multi-line remains the deferred Phase 4). Mark the SSE prerequisite item as landed (mirror how the secret-storage prerequisite was marked).

- [ ] **Step 2: Run the whole suite once more**

Run: `meson test -C build`
Expected: PASS — every suite (unit + gui) green.

- [ ] **Step 3: Commit**

```bash
git add NOTES.md
git commit -m "docs: note SSE streaming landed"
```

---

## Self-Review

**Spec coverage:**
- §2 SSE parser → Task 1. ✓
- §2 `[DONE]` not special-cased in parser, skipped by backend → Task 1 (passthrough test) + Tasks 4/5 (`g_strcmp0(payload,"[DONE]")`/`done_marker`). ✓
- §3 streaming transport (send_async, read loop, cancellable, non-2xx error body) → Task 3. ✓
- §3 `_llm_ghost_http_parse_json` → Task 3. ✓
- §4 `partial-data` signal + emit helper + name macro → Task 2. ✓
- §5 OpenAI: stream field/default, body `stream` flag, lenient delta extractor (chat/completions/role-only/finish/error), streaming flow, opt-out → Task 4. ✓
- §6 generic: `stream`/`stream_path`/`done_marker`/`stream_field`, lenient extractor, `stream_field` body override (incl. ""=untouched), streaming flow → Task 5. ✓
- Settings/construction (factory maps config to setters; default-on; opt-out) → Task 6. ✓
- §8 controller wiring + in-flight gating → Task 7. ✓
- §9 error handling (non-2xx, mid-stream drop, error member, malformed skip, cancel, empty) → covered across Tasks 3 (non-2xx), 4 (error member, malformed skip), 7 (gating); cancel uses the existing controller path; empty completion returns "" handled by existing `on_completion_ready`. ✓
- §10 testing (parser unit, delta extractors, settings/config, loopback SoupServer, controller incremental) → Tasks 1,3,4,5,7. Settings parsing of the new keys is covered structurally by the factory + integration tests rather than a standalone settings test, since the keys are read by the factory (not `llmghost-settings.c`); noted in Task 6. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code; every run step shows the exact command + expected result. ✓

**Type consistency:** `_llm_ghost_http_post_json_stream_async`/`_finish`, `LlmGhostSseEventFn`, `_llm_ghost_http_parse_json`, `_llm_ghost_sse_parser_{new,free,feed,finish}`, `LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA`, `_llm_ghost_backend_emit_partial_data`, `_llm_ghost_openai_extract_delta` / `_set_stream`, `_llm_ghost_generic_extract_delta` / `_build_body_with_stream` / `_backend_set_streaming` are used identically across declaration, implementation, and tests. The two body-builder signatures gain the same trailing `gboolean stream` everywhere they're called (backend + `test-openai-body.c`). ✓

**One refinement vs. the spec:** the spec listed `lib/llmghost-settings.c` among modified files; in practice the new backend config keys are parsed by the **factory** (`build_openai`/`build_generic`) from the backend params object — `llmghost-settings.c` needs no change (interpolation already runs over the whole tree). This is a simplification, not a scope change.
