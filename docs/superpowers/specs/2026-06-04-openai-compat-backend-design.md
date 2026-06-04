# OpenAI-Compatible Backend — Design Spec

**Date:** 2026-06-04
**Phase:** 6 (cloud / OpenAI-compatible LLM provider backends) — first backend.
**Status:** Approved (design gates cleared; in-process decisions delegated to the implementer).

## Goal

Add `LlmGhostOpenAIBackend`, a `LlmGhostBackend` implementation that talks to
any **OpenAI-compatible** HTTP endpoint — cloud OpenAI, or local/self-hosted
servers (LM Studio, llama.cpp server, vLLM). It supports two request modes
selectable at construction:

- **`COMPLETIONS`** — the legacy `/v1/completions` endpoint with the native
  `suffix` field = true fill-in-middle (FIM).
- **`CHAT`** — `/v1/chat/completions` with a prompt-engineered FIM instruction
  (the chat endpoints have no native FIM).

As the **second HTTP backend**, this work also extracts the shared libsoup +
json-glib plumbing out of the Ollama backend into a reusable
`llmghost-http-util`, then builds the new backend on top of it (decision:
extract-first).

## Non-goals (explicitly deferred)

Each is its own later step per `NOTES.md` Phase 6 sequencing:

- GSettings schema / preferences UI (backend & per-backend param selection).
- libsecret secret storage — the API key is read from an env var for now.
- SSE streaming (`partial-data` signal). Non-streaming only.
- The Mistral (Codestral) and Claude backends.
- Wiring backend selection into the **gedit plugin** layer (it stays hardcoded
  to Ollama; the settings UI will own selection). Reachability for this cut is
  via the demo app only.

## Architecture

### Component 1 — `llmghost-http-util` (extracted shared plumbing)

New files `lib/llmghost-http-util.{h,c}`. A two-layer async helper that owns
HTTP transport, status handling, JSON parsing, and the `SoupMessage` lifetime:

```c
/* POST json_body to url; attach "Authorization: Bearer <bearer>" iff bearer
 * is non-NULL and non-empty. Takes ownership of json_body. */
void       _llm_ghost_http_post_json_async  (SoupSession *session,
                                             const char *url,
                                             const char *bearer,      /* nullable */
                                             char *json_body,         /* owned */
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

/* Returns the parsed root JsonNode (caller owns via json_node_unref), or
 * NULL + GError on: transport failure, non-2xx HTTP (error message includes
 * the status code and a body snippet), or malformed JSON. */
JsonNode * _llm_ghost_http_post_json_finish (GAsyncResult *result,
                                             GError **error);
```

**Division of labor.** The util handles transport + HTTP status + JSON parse.
Each *backend* keeps: (a) URL + request-body construction, (b) **API-level**
error extraction, and (c) success-field extraction — because these differ
between providers:

| | Ollama | OpenAI |
|---|---|---|
| API error shape | `{"error": "msg"}` (string) | `{"error": {"message": "msg"}}` (object) |
| Success field | `response` | `choices[0].text` / `choices[0].message.content` |

The Ollama backend is refactored to call the util: its `request()` becomes
build-body → `_llm_ghost_http_post_json_async`; its response callback calls
`_finish`, checks the `error` member, and pulls `response`. The existing
`_llm_ghost_ollama_build_request_body` (and `test-ollama-body`) are unchanged.

### Component 2 — `LlmGhostOpenAIBackend`

Files mirror the Ollama backend's layout:

| File | Role |
|------|------|
| `lib/llmghost-openai-backend.h` | Public: `G_DECLARE_FINAL_TYPE`, mode enum, constructor |
| `lib/llmghost-openai-backend.c` | The `LlmGhostBackend` impl over `llmghost-http-util` |
| `lib/llmghost-openai-backend-internal.h` | Testing seam: pure body-builders + `clean_chat_completion` |

Public API:

```c
typedef enum {
  LLM_GHOST_OPENAI_MODE_COMPLETIONS,
  LLM_GHOST_OPENAI_MODE_CHAT,
} LlmGhostOpenAIMode;

LlmGhostBackend *llm_ghost_openai_backend_new (const char *base_url,  /* NULL/"" → default */
                                              const char *model,      /* NULL/"" → default */
                                              const char *api_key,    /* NULL/"" → no auth */
                                              LlmGhostOpenAIMode mode);
```

Instance state: `SoupSession *session`, `char *base_url`, `char *model`,
`char *api_key` (nullable), `LlmGhostOpenAIMode mode`, plus `guint max_tokens`
and `double temperature`.

## Request modes

Shared request params (both modes): `model`, `max_tokens` (default 64),
`temperature` (default 0.2), `stop: ["\n"]` (single-line enforcement),
`stream: false`. Both extract from `choices[0]`.

### Mode A — `COMPLETIONS` (native FIM)

```jsonc
POST {base_url}/completions
{ "model": "...", "prompt": "<prefix>", "suffix": "<suffix>",
  "max_tokens": 64, "temperature": 0.2, "stop": ["\n"], "stream": false }
```

Response: `choices[0].text`. No `LlmGhostFimTokens` machinery — the server
does FIM natively via the `suffix` field. No cleanup needed beyond the
controller's existing trailing-whitespace strip.

### Mode B — `CHAT` (prompt-engineered FIM)

```jsonc
POST {base_url}/chat/completions
{ "model": "...", "messages": [
    { "role": "system", "content":
      "You are a code completion engine. Output only the code that belongs between the given PREFIX and SUFFIX. No explanations, no markdown fences, no repetition of the prefix or suffix." },
    { "role": "user", "content": "<PREFIX>{prefix}</PREFIX>\n<SUFFIX>{suffix}</SUFFIX>" } ],
  "max_tokens": 64, "temperature": 0.2, "stop": ["\n"], "stream": false }
```

Response: `choices[0].message.content`, passed through `clean_chat_completion()`.

**`clean_chat_completion(const char *raw)` → newly-allocated cleaned string**
(pure, in the internal header, exhaustively unit-tested):

1. Trim surrounding ASCII whitespace.
2. If the content is wrapped in a triple-backtick fence (optionally with a
   language tag), unwrap to the inner content.
3. Truncate at the first newline (defensive: compat servers don't always honor
   `stop`).
4. Return the result (may be empty → treated as "no suggestion", same as an
   empty Ollama response).

## Configuration

Constructor args take precedence; when an arg is NULL/empty the constructor
reads the env-var override; otherwise the default applies. Mirrors
`llm_ghost_ollama_backend_new`'s `LLMGHOST_OLLAMA_*` reading.

| Setting | Env override | Default |
|---------|-------------|---------|
| base_url | `LLMGHOST_OPENAI_BASE_URL` | `https://api.openai.com/v1` |
| model | `LLMGHOST_OPENAI_MODEL` | `""` (server's loaded model; required for cloud) |
| api_key | `LLMGHOST_OPENAI_API_KEY` | `NULL` (Bearer sent only when non-empty) |
| mode | `LLMGHOST_OPENAI_MODE` | `chat` (values: `chat` / `completions`) |

Zero-config (`new(NULL, NULL, NULL, CHAT)` with no env) targets cloud OpenAI
and errors until a key + model are configured — honest to the backend's name.
Local iteration: set `LLMGHOST_OPENAI_BASE_URL=http://localhost:1234/v1` (LM
Studio), no key needed.

The URL is formed by joining `base_url` + `/completions` or
`/chat/completions`, tolerating a trailing slash on `base_url`.

## Reachability

`tests/demo.c` gains a minimal `LLMGHOST_BACKEND` selector (`ollama` default,
`openai` opt-in) so the new backend is runnable end-to-end in the demo. The
gedit plugin stays hardcoded to Ollama (real selection waits for the settings
UI). Mode/base/model/key for the demo come from the `LLMGHOST_OPENAI_*` env
vars above.

## Error handling

- Transport / non-2xx / malformed JSON → surfaced by the util as a `GError`
  (status code + body snippet included), propagated through the backend's
  `request_finish`. The controller already treats a failed request as "no
  ghost", so a misconfigured key simply yields no suggestion (plus a logged
  error), never a crash.
- API-level error object in a 2xx body (`{"error": {"message": ...}}`) →
  backend returns a `GError` with that message.
- Empty completion → returned as `""` (no suggestion), consistent with Ollama.

## Testing strategy

All display-free → `unit` suite (no Xvfb). Automated tests are the correctness
gate (the developer cannot manually run the GUI demo).

1. **`test-openai-body`** — pure builders + cleanup via the internal header.
   Build JSON, parse it back with json-glib, assert structure:
   - completions body has `model`/`prompt`/`suffix`/`max_tokens`/`temperature`/
     `stop:["\n"]`/`stream:false`;
   - chat body has the two `messages` with correct roles and the prefix/suffix
     embedded in the user content;
   - `clean_chat_completion` cases: plain, fenced (``` and ```lang), multi-line
     (truncates at first newline), leading/trailing whitespace, empty input,
     fence-only.

2. **`test-http-util`** — in-process `SoupServer` on loopback drives the util:
   - correct `Content-Type: application/json` and body posted;
   - `Authorization: Bearer <key>` present iff a key is set, absent otherwise;
   - parsed JSON returned on `200`;
   - `GError` (with status + snippet) on `500`;
   - `GError` on malformed-JSON `200` body.

3. **Refactor safety net** — `test-ollama-body` (unchanged builder) and the
   `controller` gui tests stay green through the util extraction.

Manual verification (optional, by the implementer if a server is reachable):
LM Studio locally for both modes; cloud OpenAI with a key. Not required for
completion — the automated suite is authoritative.

## Build / meson

- `lib/meson.build`: add `llmghost-http-util.c` and `llmghost-openai-backend.c`
  to the library sources; add the public headers to the installed-headers list
  (the two `*-internal.h` headers are NOT installed). The library already
  depends on libsoup-3.0 and json-glib.
- `tests/meson.build`: register `test-openai-body` and `test-http-util` in the
  `unit` suite (linked against `llmghost_dep`, like `test-ollama-body`).

## Self-review

- **Placeholders:** none — every interface, field, default, and test case is
  concrete.
- **Consistency:** util seam (Component 1) matches both backends' use; mode
  enum names identical across header/impl/tests; defaults table matches the
  Configuration prose; field-extraction table matches the request-mode
  responses.
- **Scope:** single backend + one focused extraction + tests; fits one plan.
  Deferred items enumerated.
- **Ambiguity:** `clean_chat_completion` steps are ordered and explicit; URL
  join trailing-slash behavior specified; auth-header condition (non-NULL &&
  non-empty) specified; zero-config behavior specified.
