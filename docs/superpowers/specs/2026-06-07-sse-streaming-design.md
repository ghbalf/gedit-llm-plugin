# SSE Streaming — Design

**Date:** 2026-06-07
**Status:** Approved
**Feature:** Stream completion tokens into the ghost overlay to cut perceived latency on high-latency cloud backends.

## Goal

Today every backend reads the whole HTTP response body before showing anything, so the ghost overlay only appears after the full completion arrives (~500ms+ on cloud chat backends). This feature streams Server-Sent Events (SSE) so tokens render into the overlay as they arrive, while keeping `request_finish` returning the full completion string so non-streaming-aware callers are unchanged.

## Scope (v1)

- **OpenAI-compatible SSE** for the built-in OpenAI backend (covers OpenAI, Mistral, LM Studio — one wire shape: `data: {json}\n\n` framing, `data: [DONE]` sentinel, delta at `choices[0].delta.content` for chat / `choices[0].text` for completions).
- **Config-driven streaming for the generic/template backend** so users can wire arbitrary OpenAI-compatible streaming APIs from `settings.json`.

**Out of scope (deferred):** Anthropic event-stream framing, Gemini `streamGenerateContent`, Ollama newline-delimited JSON, multi-line ghost rendering (Phase 4).

## Decisions

| Decision | Choice |
|---|---|
| v1 backend scope | OpenAI-compatible + generic/template |
| Signal payload | Accumulated text-so-far (not delta chunks) — consumer stays stateless |
| Activation | Always stream when able, with a per-backend `"stream": false` opt-out (default true) |
| Mid-stream failure | `request_finish` returns a `GError`; controller clears the ghost (all-or-nothing, matches today) |
| Generic body `stream` injection | Configurable via `stream_field` (default `"stream"`); `""`/null leaves the template untouched |

## Architecture

Four layered units. Bytes flow up; text flows back through a GObject signal. The framing/extraction logic is **pure** (no I/O); the libsoup/GIO edge is **thin** — the same pure-core/thin-edge split used by `secret-store` and `settings`.

```
keystroke → controller (debounce, GCancellable)
   → backend.request()                         [builds body with stream:true]
      → http-util stream transport             [send_async → GInputStream, read loop under cancellable]
         → SSE parser                          [pure framing: bytes → complete data: payloads]
      ← per-event callback → backend extracts delta, accumulates, emits "partial-data"(accumulated)
   ← controller sets overlay ghost (single line)
   ... EOF → request_finish() returns full text   (or GError → ghost cleared)
```

### Component 1: pure SSE parser — `lib/llmghost-sse-parser.{c,h}` (internal, new)

Stateful framing-only parser. No JSON, no I/O. Turns an arbitrarily-chunked byte stream into complete `data:` payload strings.

```c
typedef struct _LlmGhostSseParser LlmGhostSseParser;

LlmGhostSseParser *_llm_ghost_sse_parser_new  (void);
void               _llm_ghost_sse_parser_free (LlmGhostSseParser *p);

/* Feed a chunk. For each COMPLETE SSE event, append its assembled "data:"
 * payload (newly-allocated char*) to @out_events. Multiple data: lines in one
 * event are joined with '\n'. event:/id:/comment lines are ignored. The
 * [DONE] sentinel is emitted as an ordinary payload — the parser does NOT
 * interpret it. Trailing incomplete bytes are retained until the next feed. */
void _llm_ghost_sse_parser_feed   (LlmGhostSseParser *p, const char *data,
                                   gsize len, GPtrArray *out_events);

/* Flush any buffered final event when the stream ends without a trailing
 * blank line. */
void _llm_ghost_sse_parser_finish (LlmGhostSseParser *p, GPtrArray *out_events);
```

`[DONE]` is intentionally **not** special-cased — the parser stays provider-agnostic and the backend decides the sentinel. This isolates framing reassembly (the bug-prone part) as a pure function exhaustively unit-testable with hand-split byte streams.

Framing rules: line-buffer the input (split on `\n`, strip a trailing `\r`); a blank line ends an event and emits the accumulated `data:` payload; `data:` lines have the prefix and one optional leading space stripped, and multiple within one event join with `\n`; `event:`/`id:`/`retry:`/comment (`:`) lines are ignored.

### Component 2: streaming transport — `lib/llmghost-http-util.{c,h}` (modified)

One new async pair alongside the existing whole-body calls. Takes a headers object (each backend supplies its own auth) and invokes a per-event callback as payloads arrive.

```c
typedef void (*LlmGhostSseEventFn) (const char *payload, gpointer user_data);

void     _llm_ghost_http_post_json_stream_async  (SoupSession *session, const char *url,
                                                  JsonObject *headers /*nullable*/, const char *body,
                                                  LlmGhostSseEventFn on_event, gpointer event_data,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback, gpointer user_data);
gboolean _llm_ghost_http_post_json_stream_finish (GAsyncResult *result, GError **error);
```

Internals: `soup_session_send_async` → `GInputStream`; a `g_input_stream_read_async` loop feeds each chunk to an `LlmGhostSseParser` and dispatches each payload to `on_event`, checking the `GCancellable` each iteration. On non-2xx status it reads the (small) error body and finishes with `G_IO_ERROR_FAILED` + message; on read error or cancel it finishes with that error; on EOF it calls `parser_finish` then completes successfully.

### Component 3: the `partial-data` signal — `lib/llmghost-backend.{c,h}` + `lib/llmghost-backend-internal.h` (new)

GObject signals can live on a GInterface; registered once in the interface `default_init`.

```c
/* public header — canonical name + documented signature */
#define LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA "partial-data"
/* signature: (LlmGhostBackend *self, const char *accumulated_text) */
```
```c
/* llmghost-backend-internal.h (new, NOT installed) */
void _llm_ghost_backend_emit_partial_data (LlmGhostBackend *self, const char *accumulated);
```

`request_finish` still returns the full completion, so non-streaming-aware callers are unchanged. Backends that never stream simply never emit. The controller opts in by connecting the signal.

### Component 4: OpenAI streaming — `lib/llmghost-openai-backend.{c,-internal.h}` (modified)

- New `gboolean stream` field (default `TRUE`). The two body builders gain a `stream` parameter so they emit `"stream": true/false` (replacing the hardcoded `FALSE`).
- New lenient per-event delta extractor:
  ```c
  /* "" when this event carries no content delta (role-only opener, finish
   * chunk). Never errors on a missing field. chat → choices[0].delta.content;
   * completions → choices[0].text. Surfaces an "error" member as a GError. */
  char *_llm_ghost_openai_extract_delta (JsonNode *event, LlmGhostOpenAIMode mode, GError **error);
  ```
- Streaming flow: build body with `stream:true`; build an `Authorization: Bearer <key>` headers object; call the stream transport. The per-event callback skips the `[DONE]` sentinel and unparseable/empty payloads, parses the JSON, extracts the delta, appends to a `GString` accumulator, and emits `partial-data` with the cleaned single-line accumulation. On finish, returns the accumulator cleaned to match the existing whole-body cleaning per mode. When `stream` is false, the existing whole-body path runs unchanged.

### Component 5: generic streaming — `lib/llmghost-generic-backend.{c,-internal.h}` (modified)

New optional config fields:
- `stream` (bool, default `true`) — opt-out.
- `stream_path` (string) — dotted path to the delta in each event (e.g. `choices.0.delta.content`); reuses the existing index-aware path grammar.
- `done_marker` (string, default `[DONE]`) — sentinel payload to skip.
- `stream_field` (string, default `"stream"`) — top-level body member the backend sets to the streaming boolean; `""`/null leaves the request template byte-for-byte untouched.

**Streaming is active iff `stream_path` is set AND `stream != false`.** When active, the backend sets the body member named by `stream_field` to `true` (so the wire request and our parsing can't disagree) unless `stream_field` is empty.

- New lenient extractor (the strict `_llm_ghost_generic_extract` errors on a missing path, which is wrong for role/finish events):
  ```c
  /* Like _extract but returns "" when @path is absent/non-string in @event. */
  char *_llm_ghost_generic_extract_delta (JsonNode *event, const char *path);
  ```
- Flow mirrors OpenAI: stream transport with the configured headers; per-event callback skips `done_marker`/unparseable payloads, extracts the delta via the lenient path, accumulates, emits. Non-streaming config → existing whole-body path unchanged.

### Settings & construction

The backend factory passes the new fields to the constructors: OpenAI `_new` gains `stream`; generic `_new` gains `stream`, `stream_path`, `done_marker`, `stream_field`. Settings parsing reads `"stream"` (default true), `"stream_path"`, `"done_marker"`, `"stream_field"` from each backend's config object. `${ENV}`/`${secret:NAME}` interpolation is untouched.

### Controller wiring — `lib/llmghost-controller.c` (modified)

After constructing the backend, connect once: `g_signal_connect (backend, "partial-data", on_partial_data, self)`. The handler sets the overlay ghost to the accumulated string (single line for now — multi-line is the deferred Phase 4). To prevent a late emission from a cancelled request bleeding into a newer one, the handler **only updates the overlay while a request is currently in-flight** (gated on the controller's existing current-request state). The existing debounce + cancel-on-keystroke path is unchanged; a mid-stream cancel aborts the read loop, and `request_finish` returns `G_IO_ERROR_CANCELLED`, which the controller already ignores.

## Error handling

| Situation | Behavior |
|---|---|
| HTTP non-2xx | transport finishes with `GError` → `request_finish` errors → ghost cleared |
| Mid-stream connection drop | read error → `GError` → ghost cleared |
| `error` member in an event (OpenAI) | extractor sets `GError` → stream aborts → ghost cleared |
| One malformed/empty event line | skipped, stream continues |
| Cancelled (new keystroke) | read loop aborts → `CANCELLED` → controller ignores |
| Empty completion | returns `""` → nothing shown |

## Testing

1. **`tests/test-sse-parser.c`** (pure, new): event split across chunks; multiple events per chunk; CRLF; `event:`/comment lines ignored; multi-`data:` concatenation; `[DONE]` passed through; trailing-partial via `finish()`.
2. **OpenAI delta extractor** (`test-openai-backend`): chat path, completions path, role-only → `""`, finish chunk → `""`, `error` member → GError; plus body-builder `stream:true/false`.
3. **Generic delta extractor + body override** (`test-generic-backend`): lenient missing path → `""`, present/index paths; `stream_field` body override (set, default, disabled).
4. **Settings** (`test-settings`): `stream` default true, `stream_path`, `done_marker`, `stream_field` parsed.
5. **Loopback `SoupServer`** (existing http-util harness style): chunked `text/event-stream` with several events + `[DONE]` → assert ordered delivery + success; server that drops mid-stream → finish fails.
6. **Controller incremental render** (xvfb gui suite): a fake in-process backend emitting `partial-data` several times → assert the overlay grows and the final equals the full text; assert a mid-stream cancel leaves the overlay cleared. No network — pure GObject fake.

## File inventory

**New:** `lib/llmghost-sse-parser.{c,h}`, `lib/llmghost-backend-internal.h`, `tests/test-sse-parser.c`.

**Modified:** `lib/llmghost-http-util.{c,h}`, `lib/llmghost-backend.{c,h}`, `lib/llmghost-openai-backend.{c,-internal.h}`, `lib/llmghost-generic-backend.{c,-internal.h}`, `lib/llmghost-settings.c` (+ backend factory), `lib/llmghost-controller.c`, `lib/meson.build`, `tests/meson.build`, `NOTES.md`, and the affected test files.

Internal headers go in sources only — never added to the installed `llmghost_headers`.
