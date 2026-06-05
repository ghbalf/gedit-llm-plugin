# Generic (Template) Backend — Design Spec

**Date:** 2026-06-05
**Phase:** 6 (backends), follow-on to the JSON settings layer.
**Status:** Draft for review. Brainstorm decisions: v1 target = Anthropic + Gemini native; response cleanup = always (shared helper); transport = generalize the HTTP util's header model (approach A). Awaiting user review before the plan.

## Goal

Let a user point the plugin at **non-OpenAI-shaped** LLM completion APIs — Anthropic's native Messages API, Google Gemini's native API, and similar — purely by writing JSON config, with no new C code per provider. The user supplies a request-body **template**, the set of **HTTP headers**, the **URL**, and a **response path**; the backend fills the template with the per-request FIM context, POSTs it, and extracts the completion.

This builds directly on the JSON settings layer: the template and headers are inherently file-shaped (a multi-line JSON body does not fit an env var), and `${ENV_VAR}` interpolation already runs over the whole stanza at load time, so secrets and key-in-URL auth need no backend code.

### What this is NOT
It does **not** replace the OpenAI-compat backend, which already covers every OpenAI-*shaped* provider (OpenRouter, Groq, Together, Fireworks, vLLM, LM Studio, llama.cpp, Ollama-compat, …) with native FIM via `suffix` and simpler config. The generic backend's unique value is the **non-OpenAI shapes**. Both coexist; the user picks per provider.

## v1 success criteria

Working, shipped `settings.json` templates for **both**:
1. **Anthropic** native Messages API — `x-api-key` + `anthropic-version` headers (NOT Bearer), response at `content.0.text`.
2. **Gemini** native API — API key in the URL query string (`?key=${GEMINI_API_KEY}`), response at `candidates.0.content.parts.0.text`.

These two drive every requirement (arbitrary headers, key-in-URL, array-indexed nested response paths). The pure substitution + extraction + cleanup logic is exhaustively unit-tested; the custom-header transport is covered by an in-process `SoupServer` loopback test. Only the live call to the real Anthropic/Gemini endpoints is manual (network + secret), and is not the correctness gate.

## Architecture

Four units. Three are pure/transport library code with full automated tests; the fourth is a tiny shared extraction.

| Unit | Responsibility |
|------|----------------|
| HTTP util generalization (`lib/llmghost-http-util.{c,h}`) | Add an arbitrary-headers POST core; reimplement the existing Bearer call as a thin wrapper over it. |
| Shared single-line cleanup (`lib/llmghost-text-util.{c,h}`) | Extract the OpenAI chat-cleanup (fence-strip + first non-empty line) into a shared internal helper used by both the OpenAI and generic backends. |
| `LlmGhostGenericBackend` (`lib/llmghost-generic-backend.{c,h}` + `-internal.h`) | Implements `LlmGhostBackend`. Holds a self-contained copy of url/headers/model/request_template/response_path. Per request: build body → POST with headers → extract by path → clean → return. |
| Factory branch (`lib/llmghost-backend-factory.c`) | A `build_generic` branch + a `"generic"` dispatch case. |

### Transport: approach A (generalize the core, keep the Bearer wrapper)

Add to `lib/llmghost-http-util.{c,h}`:

```c
/* POST @json_body to @url with the given request @headers (each member of the
 * JsonObject is sent as "name: value"; values must be strings). Always sets
 * Content-Type: application/json unless @headers overrides it. Takes ownership
 * of @json_body. @session, @cancellable, @headers are borrowed. */
void _llm_ghost_http_post_json_headers_async (SoupSession         *session,
                                              const char          *url,
                                              JsonObject          *headers,
                                              char                *json_body,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);
```

The existing `_llm_ghost_http_post_json_async(session, url, bearer, body, …)` is **reimplemented** to build a one-entry headers object (`{"Authorization": "Bearer <bearer>"}` when bearer is non-empty, else empty) and delegate to the new core. The shared `_llm_ghost_http_post_json_finish` is unchanged and used by both. **The three existing backends (ollama, openai, mistral) are not touched** — they keep calling the Bearer wrapper, which keeps behaving identically.

Header values are sent verbatim via `soup_message_headers_append`. Non-string header values in config are skipped with a warning (type-guarded, like the factory's `param_string`).

### Why structural template substitution (not text substitution)

The request body is JSON. The FIM prefix/suffix routinely contain characters that are significant inside JSON strings — double quotes, backslashes, and (critically) newlines. Naively replacing `{{prefix}}` with the raw prefix text inside the serialized JSON would produce invalid JSON the moment the prefix contains a `"` or newline.

Instead: the template is stored as a parsed `JsonObject`. To build a request we **deep-copy the template tree, walk every string value, substitute the `{{…}}` tokens, then re-serialize via json-glib** — which escapes quotes/backslashes/newlines correctly and automatically. The placeholder values are inserted as plain string content; quoting is the serializer's job.

## The two pure functions (declared in `-internal.h`, the test seam)

```c
/* Deep-copy @template, replace {{prefix}}/{{suffix}}/{{model}} inside every
 * string value (single left-to-right pass per string — inserted content is
 * never re-scanned), and serialise to a newly-allocated JSON string. */
char *_llm_ghost_generic_build_body (JsonObject *template,
                                     const char *prefix,
                                     const char *suffix,
                                     const char *model);

/* Walk a dotted @path ("content.0.text", "candidates.0.content.parts.0.text")
 * through @root: an object segment selects a member; an all-digits segment
 * indexes an array. Returns the located string (newly-allocated), or NULL +
 * @error if the path does not resolve to a string. */
char *_llm_ghost_generic_extract (JsonNode    *root,
                                  const char  *path,
                                  GError     **error);
```

**Placeholders (request-time):** exactly `{{prefix}}`, `{{suffix}}`, `{{model}}`. `prefix`/`suffix` are the FIM context from the controller; `model` is the stanza's `"model"` field (convenience so a user can switch models without editing the template body). Unknown `{{…}}` tokens are left verbatim (no error — forward-compatible).

**Single-pass safety:** substitution scans each template string once, emitting either a matched placeholder's value or the literal character. A prefix whose text happens to contain `{{suffix}}` is inserted as data and not re-substituted.

**`response_path` grammar:** segments split on `.`. A segment of all ASCII digits indexes the current node as an array; any other segment selects an object member. Type mismatches (index into an object, member of an array, a non-string leaf, out-of-range index, missing member) → `NULL` + a `G_IO_ERROR_FAILED` error whose message names the failing segment.

## Response cleanup (shared)

Extract the OpenAI backend's `_llm_ghost_openai_clean_chat_completion` (currently in `lib/llmghost-openai-backend.c`, exposed via `lib/llmghost-openai-backend-internal.h`, tested in `tests/test-openai-body.c`) into a new shared internal helper:

```c
/* Strip a single leading/trailing markdown code fence and return the first
 * non-empty line, trimmed. For turning chat-model prose into ghost text. */
char *_llm_ghost_clean_single_line (const char *raw);   /* lib/llmghost-text-util.h */
```

The OpenAI backend is refactored to call the shared helper (its existing test stays green — behavior is identical). The generic backend always applies it to the extracted completion. One behavior, one implementation, one test.

## Config schema (the `generic` stanza)

Activated by `"backend": "generic"`. Lives under `backends.generic` like every other backend; the settings parser already ignores unknown keys, so **no settings-layer code changes** are needed.

```jsonc
"backend": "generic",
"backends": {
  "generic": {
    "url": "https://api.anthropic.com/v1/messages",
    "headers": {
      "x-api-key": "${ANTHROPIC_API_KEY}",
      "anthropic-version": "2023-06-01"
    },
    "model": "claude-3-5-haiku-20241022",
    "request_template": {
      "model": "{{model}}",
      "max_tokens": 64,
      "messages": [
        { "role": "user",
          "content": "You are a code completion engine. Continue the code at the cursor; output only the completion text, no prose, no code fences.\n<before_cursor>{{prefix}}</before_cursor>\n<after_cursor>{{suffix}}</after_cursor>" }
      ]
    },
    "response_path": "content.0.text"
  }
}
```

- `url` — required. May embed `${ENV}` (covers Gemini's `?key=${GEMINI_API_KEY}`).
- `headers` — optional object of string→string. `${ENV}` interpolated at load. Sent verbatim.
- `model` — optional string; the value substituted for `{{model}}`.
- `request_template` — required JSON object; the request body shape with `{{…}}` placeholders.
- `response_path` — required dotted path string.

`${ENV}` (settings, load-time) and `{{…}}` (backend, request-time) use distinct delimiters and never collide.

## Factory wiring

`build_generic(JsonObject *params)` in `lib/llmghost-backend-factory.c`:
- reads `url` / `response_path` (strings), `model` (string, optional), `headers` and `request_template` (objects),
- **deep-copies** `headers` and `request_template` into the backend so it is self-contained and survives a settings live-reload (which rebuilds the settings tree and reconstructs the backend),
- constructs `llm_ghost_generic_backend_new(url, headers, model, request_template, response_path)`.

Dispatch: a `"generic"` case alongside openai/mistral; the existing unknown→ollama fallback is unchanged.

## Backend request flow

`LlmGhostGenericBackend` implements the interface like the Mistral backend:
1. `request()` — `_llm_ghost_generic_build_body(self->template, prefix, suffix, self->model)` → POST to `self->url` with `self->headers` via `_llm_ghost_http_post_json_headers_async`.
2. completion callback — `_llm_ghost_http_post_json_finish` → `_llm_ghost_generic_extract(root, self->response_path, &err)` → `_llm_ghost_clean_single_line(extracted)` → return via the GTask.

Owns a `SoupSession` (30 s timeout, as the other HTTP backends), `url`, `headers` (JsonObject), `model`, `request_template` (JsonObject), `response_path`; frees all in finalize with `g_clear_*`. Builders/extractors use the leak-free json-glib idioms already established (ref-before-unref-parser; `json_node_unref` the builder root).

## Error handling

- **Construction with a missing required field** (`url`, `request_template`, or `response_path`): the backend is still constructed (so the plugin never crashes) but logs a clear `g_warning` naming the missing field, and every `request()` fails fast with an error — the controller simply shows no ghost text.
- **Bad `response_path` at runtime** (wrong shape / out of range / non-string leaf): `NULL` + logged error; no ghost text; no crash.
- **Transport / non-2xx / malformed JSON**: handled by the shared `_llm_ghost_http_post_json_finish` exactly as for the other backends (status code + body snippet in the error).
- **Non-string header value in config**: skipped + warning.
- An empty extracted completion → `""` (no ghost text), not an error.

## Testing strategy

Automated tests are the correctness gate; this feature is almost entirely auto-testable (no GUI surface).

**`test-generic-body` (unit, display-free):**
- `_llm_ghost_generic_build_body`: placeholder substitution into a flat object; into nested objects/arrays; correct JSON escaping when prefix/suffix contain `"`, `\`, and newlines; `{{model}}` substitution; an unknown `{{token}}` left verbatim; the safety case where the prefix value contains `{{suffix}}` and must NOT be re-substituted; a template with no placeholders round-trips unchanged.
- `_llm_ghost_generic_extract`: object leaf; single array index; the two real nested paths (`content.0.text`, `candidates.0.content.parts.0.text`); missing member → error; out-of-range index → error; index-into-object and member-of-array → error; non-string leaf → error.

**`test-text-util` (unit):** the shared `_llm_ghost_clean_single_line` — plain text, fenced block, leading/trailing blank lines, multi-line→first line, already-clean input. (The OpenAI body test continues to pass through its refactored call.)

**HTTP transport (unit, in-process `SoupServer` loopback):** extend the existing `test-http-util` (or a sibling) to POST via `_llm_ghost_http_post_json_headers_async` with a multi-header object and assert the server received each custom header (e.g. `x-api-key`, `anthropic-version`) and the default `Content-Type: application/json`; and that the Bearer wrapper still sends `Authorization: Bearer …` through the new core.

**Factory (extend `test-settings.c`):** `"backend":"generic"` with a minimal valid stanza → `llm_ghost_backend_new_from_settings` returns a non-NULL backend of GType `LLM_GHOST_GENERIC_BACKEND`.

**Warning-determinism:** any test whose path logs a warning (missing-field construction, factory) follows the established convention — `g_unsetenv` the relevant `${ENV}` vars and pin warnings with `g_test_expect_message` so the suite is robust to exported keys and `G_DEBUG=fatal-warnings`.

**Manual (not the gate):** paste `examples/anthropic.json` / `examples/gemini.json` into `settings.json` with a real key and confirm completions against the live endpoints.

## File structure / build

- **New:** `lib/llmghost-generic-backend.{c,h}` + `-internal.h`, `lib/llmghost-text-util.{c,h}`, `tests/test-generic-body.c`, `tests/test-text-util.c`, `examples/anthropic.json`, `examples/gemini.json`.
- **Modify:** `lib/llmghost-http-util.{c,h}` (new headers core + Bearer wrapper), `lib/llmghost-openai-backend.c` + `-internal.h` (call the shared cleanup; drop the local copy), `lib/llmghost-backend-factory.c` (`build_generic` + dispatch), `lib/llmghost.h` (umbrella: generic backend header), `lib/meson.build` (sources + installed headers; `text-util.h` and the `-internal.h` are NOT installed), `tests/meson.build` (register `test-generic-body`, `test-text-util`; extend the http-util and settings tests), `tests/demo.c` (a `LLMGHOST_BACKEND=generic` selector is **out of scope** — the generic backend is config-file-driven, not env-driven; note in docs), `NOTES.md` (document the generic backend + point at `examples/`).
- The factory remains the single point coupling all backend types.

## Non-goals (deferred)

- A `LLMGHOST_BACKEND=generic` demo selector (the demo is env-configured; the generic backend is file-configured by design).
- GET requests / non-JSON bodies / non-JSON responses (all v1 targets are JSON-over-POST).
- SSE streaming (deferred project-wide).
- Per-request retry/backoff, response transforms beyond single-line cleanup, placeholders beyond `{{prefix}}`/`{{suffix}}`/`{{model}}`.
- Subsuming/removing the dedicated OpenAI/Mistral backends (they stay; a future Claude/Gemini *native* hand-written backend is now unnecessary — the templates cover them).

## Self-review

- **Placeholders:** none — schema, both pure-function grammars, the transport signature, cleanup behavior, error cases, and every test case are concrete.
- **Consistency:** the backend mirrors the Mistral backend's structure (interface impl, SoupSession, `-internal.h` seam, leak-free json idioms); the factory branch mirrors `build_openai`/`build_mistral`; the headers core preserves the existing Bearer call's behavior byte-for-byte; tests follow the established `g_test` + warning-determinism conventions.
- **Scope:** one cohesive subsystem (one new backend + a transport generalization + a small shared extraction) → one plan, ~7 tasks (headers core; shared cleanup extraction; build_body; extract; backend type; factory+settings wiring; examples+docs).
- **Ambiguity:** substitution is single-pass on string values only; response-path digit-segments index arrays; `${ENV}` vs `{{…}}` separation is explicit; deep-copy-at-construction guarantees live-reload safety; missing-field construction warns-and-degrades rather than failing to construct.
