# Mistral Codestral Backend — Design Spec

**Date:** 2026-06-04
**Phase:** 6 (cloud / OpenAI-compatible LLM provider backends) — second backend.
**Status:** Approved (Approach A; in-process decisions delegated to the implementer).

## Goal

Add `LlmGhostMistralBackend`, a `LlmGhostBackend` implementation that talks to
Mistral's **Codestral FIM** endpoint (`POST {base}/fim/completions`) — a
dedicated fill-in-middle API with native `prompt` + `suffix` fields. It is the
strongest free-tier cloud FIM option. Built on the existing shared
`llmghost-http-util`.

## Approach

**Separate backend** (not a third mode on `LlmGhostOpenAIBackend`). Codestral's
FIM differs from OpenAI's COMPLETIONS mode in endpoint path
(`/fim/completions`), response field (`choices[0].message.content`, chat-style),
and provider defaults/auth. A separate type keeps each provider's semantics
isolated, matching the project's one-backend-per-provider philosophy. The ~20
lines of request-body overlap with the OpenAI completions builder are **not**
extracted into a shared helper — that would be speculative; revisit only if a
third call site appears.

## Non-goals (deferred)

- **Settings layer** — its own later feature. **Decision recorded here:** when
  built, it will be a **human-editable JSON config file** (e.g.
  `~/.config/llmghost/settings.json`, XDG-based, parsed with json-glib, watched
  with `GFileMonitor` for reload), **not GSettings/dconf**. Rationale: this is a
  gedit plugin, so users edit the config in gedit itself; no schema-compile or
  `dconf-editor` step. The backend constructors (`*_backend_new(base, model,
  key, ...)`) are exactly what that loader will call, so this backend is already
  forward-compatible. `NOTES.md` Phase 6 "Architectural prerequisites" is
  updated to reflect this.
- libsecret secret storage (API key via env var for now).
- SSE streaming.
- The Claude (chat-FIM) backend.
- Plugin-layer backend selection (the gedit plugin stays hardcoded to Ollama;
  reachability for this cut is the demo only).

## Architecture

`LlmGhostMistralBackend` is a `GObject` implementing `LlmGhostBackend`, driving
`llmghost-http-util` for transport. Files mirror the OpenAI backend's layout:

| File | Role |
|------|------|
| `lib/llmghost-mistral-backend.h` | Public (installed): `G_DECLARE_FINAL_TYPE`, constructor |
| `lib/llmghost-mistral-backend.c` | Pure builder/extractor + GObject over the util |
| `lib/llmghost-mistral-backend-internal.h` | Testing seam (NOT installed): the two pure functions |
| `tests/test-mistral-body.c` | Unit tests for the pure functions |

Instance state: `SoupSession *session`, `char *base_url`, `char *model`,
`char *api_key` (nullable), `guint max_tokens`, `double temperature`. No mode
enum — a single FIM endpoint.

## Public API

```c
#define LLM_GHOST_TYPE_MISTRAL_BACKEND (llm_ghost_mistral_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostMistralBackend, llm_ghost_mistral_backend,
                      LLM_GHOST, MISTRAL_BACKEND, GObject)

/**
 * llm_ghost_mistral_backend_new:
 * @base_url: API base. NULL/"" → $LLMGHOST_MISTRAL_BASE_URL or the Codestral default.
 * @model:    model id. NULL/"" → $LLMGHOST_MISTRAL_MODEL or "codestral-latest".
 * @api_key:  bearer token. NULL/"" → $LLMGHOST_MISTRAL_API_KEY or no auth.
 */
LlmGhostBackend *llm_ghost_mistral_backend_new (const char *base_url,
                                               const char *model,
                                               const char *api_key);
```

## Request

`POST {base_url}/fim/completions` with `Authorization: Bearer <key>` (attached
only when a key is set; the shared util already handles the iff-non-empty rule).

```jsonc
{ "model": "codestral-latest", "prompt": "<prefix>", "suffix": "<suffix>",
  "max_tokens": 64, "temperature": 0.2, "stop": ["\n"] }
```

The URL is `base_url` joined with `fim/completions`, tolerating a trailing slash
on `base_url` (same `join_url` helper logic as the OpenAI backend, local to this
file).

## Response extraction

Pure function `_llm_ghost_mistral_extract_completion(JsonNode *root, GError **error)`:

1. Object check (else GError "mistral: malformed response").
2. If a top-level `error` member is present, return GError with its `message`
   (object form `{"error":{"message":...}}`) or the bare string — defensive,
   since transport/HTTP errors are already surfaced by the util as GError.
3. No/empty `choices` → `g_strdup("")` (no suggestion).
4. From `choices[0]`: prefer `message.content`; **fall back** to `text` if
   `message` is absent. This tolerates either response shape without live-API
   verification.
5. Return a newly-allocated copy (default `""`). No chat-style fence cleanup — a
   FIM endpoint returns the raw middle, not prose.

## Configuration

Constructor args take precedence; when an arg is NULL/empty the constructor
reads the env override; else the default. Mirrors the OpenAI/Ollama constructors.

| Setting | Env override | Default |
|---------|-------------|---------|
| base_url | `LLMGHOST_MISTRAL_BASE_URL` | `https://codestral.mistral.ai/v1` |
| model | `LLMGHOST_MISTRAL_MODEL` | `codestral-latest` |
| api_key | `LLMGHOST_MISTRAL_API_KEY` | `NULL` (Bearer sent only when set) |

Defaults `max_tokens=64`, `temperature=0.2`, request timeout 30s (as in the
other HTTP backends). Zero-config targets Codestral and errors until a key is
set — honest to the backend's name.

## Reachability

Extend the demo's existing `LLMGHOST_BACKEND` selector with `mistral`
(`ollama` default, `openai`, `mistral`). Config via the `LLMGHOST_MISTRAL_*`
env vars. The gedit plugin stays hardcoded to Ollama.

## Error handling

- Transport / non-2xx / malformed JSON → surfaced by the util as a GError
  (status + snippet), propagated through `request_finish`. The controller treats
  a failed request as "no ghost", so a missing/invalid key yields no suggestion
  (plus a logged error), never a crash.
- API-level error object in a 2xx body → GError with its message.
- Empty completion → `""` (no suggestion), consistent with the other backends.

## Testing strategy

Display-free → `unit` suite. Automated tests are the correctness gate (the
developer cannot run the GUI).

**`test-mistral-body`** — pure functions via the internal header:
- `_llm_ghost_mistral_build_fim_body`: build JSON, parse it back, assert
  `model`/`prompt`/`suffix`/`max_tokens`/`temperature`/`stop:["\n"]`.
- `_llm_ghost_mistral_extract_completion`: `message.content` happy path; the
  `text` fallback (no `message`); empty `choices`; missing `choices`; error
  object.

The HTTP transport path is already covered by `http-util`'s loopback test; the
backend's thin async glue mirrors the (also-untested-but-trivial) OpenAI/Ollama
glue. Manual verification against the live Codestral API is optional and not
required for completion.

## Build / meson

- `lib/meson.build`: add `llmghost-mistral-backend.c` to `llmghost_sources`;
  add `llmghost-mistral-backend.h` to `llmghost_headers` (installed). The two
  `*-internal.h` / util headers stay non-installed.
- `tests/meson.build`: register `test-mistral-body` in the `unit` suite.
- `lib/llmghost.h`: include the new public header.

## Self-review

- **Placeholders:** none — every field, default, and test case is concrete.
- **Consistency:** constructor/defaults table matches the Configuration prose;
  extraction steps match the request/response shapes; file layout matches the
  OpenAI backend's installed/non-installed split.
- **Scope:** single backend + tests + demo hook; settings explicitly deferred
  with the direction recorded. Fits one plan.
- **Ambiguity:** response extraction order (message.content → text fallback) is
  explicit; URL trailing-slash join specified; auth iff-key handled by the util;
  zero-config behavior specified.
