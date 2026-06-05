# Working notes

Informal scratchpad for the project. Not user docs — these are running notes
about what's done, what's queued, and what's deliberately deferred so it
isn't forgotten.

## Project goal

A gedit 46 plugin (in C) that adds Supermaven-style inline LLM completion
to gedit's text view. Built bottom-up:

1. `libllmghost` — standalone GObject library that attaches to any
   `GtkTextView` and renders single-line ghost text from a pluggable async
   backend. Verifiable in a tiny demo app, no gedit needed. ✓
2. Backends behind `LlmGhostBackend` GInterface (currently: fake, Ollama
   with data-driven FIM token sets covering Qwen/StarCoder/DeepSeek). ✓
3. gedit plugin layer wrapping the library against `GeditWindow` /
   `GeditView` via libpeas 1.0. ✓

**Working state as of 2026-05-02:** The plugin loads in gedit 46.2,
shows ghost completions next to the cursor, Tab-accepts, Esc-dismisses,
follows the cursor through scrolling. Default backend is Ollama on
spark-2448 with `qwen3-coder-next:latest`. Switch model + token-set via
`LLMGHOST_OLLAMA_MODEL` + `LLMGHOST_OLLAMA_TOKENS` env vars (rebuild not
needed; `killall gedit && gedit` to reload the plugin).

**Partial acceptance landed 2026-06-03** (PR #1, merged to `master`):
Right=accept-next-char and Ctrl+Right=accept-next-word now sit alongside
Tab/Esc — the unaccepted remainder stays ghosted. Word = leading
whitespace + word-char run (alnum/`_`), else one punctuation char. Built
spec-first; design + plan under `docs/superpowers/`. Covered by the
`ghost-accept` unit suite and the new `controller` gui subtests.

**Next focus:** cloud LLM provider backends (Phase 6) — starting with an
OpenAI-compat backend, then Codestral native FIM. (Phase 5 / Supermaven
dropped 2026-06-03 — the product is effectively defunct; see that
section.)

## Toolkit

gedit 46.2 ships **GTK 3 + libpeas 1.0 + libgedit-gtksourceview-300**
(NOT GTK4 / upstream GtkSourceView 5 as widely documented online). Library
and plugin both target GTK 3.

## Phase 1 — done (2026-05-02)

`libllmghost` static library, demo app, all polish items.

- `LlmGhostBackend` GInterface — async `request` / `request_finish`
- `LlmGhostFakeBackend` — canned-response, drives the demo when Ollama
  isn't reachable
- `LlmGhostOverlay` — `GtkLabel` subclass, greyed italic markup
- `LlmGhostController` — debounce, in-flight cancellation, Tab/Esc,
  Right=accept-next-char, Ctrl+Right=accept-next-word (word = leading
  whitespace + word-char run, else one punctuation char),
  cursor-position-aware repositioning, scroll repositioning,
  cursor-safe-for-ghost suppression, trailing-whitespace strip
- `tests/demo.c` — minimal GTK3 host

## Phase 2 — done (2026-05-02)

`LlmGhostOllamaBackend` against spark-2448, family-agnostic FIM via
data-driven `LlmGhostFimTokens` value type.

- libsoup-3.0 async POST to `/api/generate`
- json-glib request build + response parse
- `raw: true` + manual sentinel injection (Ollama's native `suffix:`
  field path is unreliable: qwen3-coder-next has no `insert` template,
  starcoder2:3b returns empty, deepseek-coder errors out)
- `\n` as a stop token to enforce single-line completions, plus
  family-specific stop sentinels from the active token set
- Env-var overrides: `LLMGHOST_OLLAMA_HOST`, `_PORT`, `_MODEL`, `_TOKENS`

**FIM token sets** — `lib/llmghost-fim-tokens.{h,c}` defines a plain
value struct (`name` + `prefix_tok` + `suffix_tok` + `middle_tok` +
`stop_tokens[]`) with copy/free + three builtins (Qwen, StarCoder,
DeepSeek). Switch via `llm_ghost_ollama_backend_set_fim_tokens()`. The
backend defaults to a copy of `qwen()`. Custom sets can be constructed
at runtime via `llm_ghost_fim_tokens_new()` — the eventual settings
dialog will likely build a `GListStore` of these.

End-to-end verified 2026-05-02 against all three families: Qwen tokens
work for both `qwen3-coder-next` (~600 ms warm) and `qwen2.5-coder:7b-base`
(~190 ms warm); StarCoder tokens work for `starcoder2:3b` (~70 ms,
clean tokens but suffix not strongly honored — model is 3B); DeepSeek
tokens work for `deepseek-coder:6.7b-base` (~100 ms warm, suffix
honored). Differential FIM test in all cases.

## Ollama-on-spark-2448 reality

Of 16 models on the host, **only `qwen3-coder-next:latest` works for FIM**.
CodeGemma 2B and Gemma 3:4B are broken in Ollama's current packaging — the
GGUF tokenizer encodes `<|fim_*|>` markers as ordinary subword tokens, so
the model emits them as literal strings instead of consuming them.
Diagnostic: completion contains the literal string `<|fim_prefix|>` →
model is broken for FIM in this packaging.

If sub-200 ms latency ever matters more than the current 600 ms warm
path, ask the spark-2448 admin to `ollama pull deepseek-coder:6.7b-base`
or `qwen2.5-coder:7b-base`. Both are reliable FIM packagings.

## Phase 3 — done (2026-05-02)

`plugin/llmghost-plugin.{h,c}` — `GeditWindowActivatable` impl, ~150
lines of libpeas 1.0 glue. One Ollama backend per `GeditWindow`,
controllers attached to each view via `g_object_set_data_full` so they
die with the view. `tab-added` listener wires up fresh tabs.

Install path: `~/.local/share/gedit/plugins/llmghost/{libllmghost.so,
llmghost.plugin}`. Meson uses `shared_module()` and a hardcoded user
install dir so `meson install -C build` doesn't need sudo.

**Coordinate-system bug fixed in `LlmGhostController`** while
verifying in gedit (didn't manifest in the demo because the demo's
view rarely scrolls). For `GTK_TEXT_WINDOW_TEXT` children, GTK 3's
`add_child_in_window`/`move_child` interpret (x,y) as **buffer
coordinates**, not window coordinates as the docs say. The fix: pass
`line_y` from `gtk_text_view_get_line_yrange()` and `cursor_rect.x`
from `gtk_text_view_get_cursor_locations()` directly — no
`buffer_to_window_coords` step. See
`memory/gtk3_text_window_child_coords.md` for the full reasoning.

`get_line_yrange()` is also load-bearing because it forces layout
validation of the cursor's line — `get_iter_location()` /
`get_cursor_locations()` can return stale or sub-line-shifted values
on a freshly-typed last line of a scrolled view.

## Phase 4 — multi-line ghost rendering (deferred)

Currently we truncate completions at the first `\n` and `GtkLabel` is
single-line. Real multi-line ghost (Copilot-style block suggestions)
needs:

- Accept multi-line into `current_ghost`.
- Either replace the `GtkLabel` with a multi-line label/box that lays out
  with the view's line-height, OR render via custom `gtk_widget_draw`.
- Care around line-wrap interaction.
- Note: with Phase 3's GTK-3-buffer-coords fix, the overlay container
  is anchored to a single buffer position; multi-line rendering will
  need to either lay out within one widget or add multiple anchored
  child widgets (one per ghost line).

Revisit once Phase 5 / 6 backends are landed and we have real signal
about which suggestions feel "wanted to span multiple lines".

## Phase 5 — Supermaven backend (DROPPED 2026-06-03)

**Dropped — Supermaven is effectively defunct as a standalone product.**
Anysphere (Cursor) acquired Supermaven in Nov 2024 and folded its
inference pipeline into Cursor's built-in Tab completion. The standalone
VS Code extension has been frozen at **v1.1.12 since ~Sept 2024** (no
updates post-acquisition), and secondary sources report the standalone
extensions were discontinued ~Nov 30 2025 (couldn't confirm the exact
date against a primary source; the marketing site still shows pricing
with no shutdown banner, but the frozen extension is the decisive tell).

Building this backend would mean reverse-engineering the proprietary
stdin/stdout protocol of an **abandoned binary** — no upstream to track,
no stability guarantee, and the binary may stop being distributable. The
sub-200 ms + huge-context edge is now a Cursor-only feature we can't call
from a gedit plugin anyway. This matches the exit criterion the original
notes already set ("undocumented/obfuscated protocol → just keep using
Ollama / cloud; Supermaven is nice-to-have, not core"). **Go straight to
Phase 6** — Codestral native FIM is the strongest free-tier path.

Original design notes kept below for the record only.

---

Supermaven is a closed-source local-first completion service known
for sub-200 ms latency. Architecture differs from Ollama in important
ways — the existing `LlmGhostBackend` GInterface stays the same, but
the implementation is process-management, not HTTP.

- **NOT HTTP**: runs as a sidecar binary (e.g. `sm-binary`) we spawn
  with `g_subprocess_new` and talk to over stdin/stdout JSON, one
  message per line.
- **Proprietary protocol**: each request gets a numeric ID; responses
  arrive in any order with the matching ID. Maintain an
  `id → GTask` table; cancellation drops the entry from the table and
  returns CANCELLED to the task.
- **Persistent process**: spawn once, keep alive across requests
  (cold start would defeat the latency win). Watch `g_subprocess_wait`
  for unexpected exits and respawn.
- **Distribution**: user must download the binary themselves; we don't
  ship it. Locate via `$LLMGHOST_SUPERMAVEN_BINARY` env var, then
  `$PATH`, then a fixed cache dir.
- **Free tier with limits, paid tier for unlimited**.

Reference for protocol: their VS Code extension is the cleanest open
documentation (their npm package or GitHub at the time of writing). If
the protocol turns out to be undocumented and obfuscated, fall back
strategy is just to keep using Ollama / cloud backends — Supermaven is
nice-to-have, not core.

## Phase 6 — cloud LLM provider backends

**OpenAI-compatible backend landed 2026-06-04.** `LlmGhostOpenAIBackend`
supports `/v1/completions` (native FIM via `suffix`) and
`/v1/chat/completions` (prompt-FIM), config via constructor +
`LLMGHOST_OPENAI_{BASE_URL,MODEL,API_KEY,MODE}`, optional Bearer auth.
Built on the new shared `llmghost-http-util` (extracted from the Ollama
backend). Reachable in the demo via `LLMGHOST_BACKEND=openai`. Covered by
the `openai-body` and `http-util` unit suites. Still deferred: libsecret key storage, SSE streaming, Claude backend.

**Mistral Codestral backend landed 2026-06-04.** `LlmGhostMistralBackend`
hits the Codestral FIM endpoint (`POST {base}/fim/completions`, native
`prompt`+`suffix`), response = `choices[0].message.content` (with a `text`
fallback). Config via constructor + `LLMGHOST_MISTRAL_{BASE_URL,MODEL,API_KEY}`,
optional Bearer auth; default base `https://codestral.mistral.ai/v1`, model
`codestral-latest`. Built on `llmghost-http-util`; reachable in the demo via
`LLMGHOST_BACKEND=mistral`. Covered by the `mistral-body` unit suite.

Three families, distinguished by FIM support:

### Native FIM
- **Mistral Codestral**: dedicated `/v1/fim/completions` with `prompt`
  + `suffix` fields. Free tier, currently the strongest cloud FIM.
- **OpenAI legacy `/v1/completions`** with `suffix:` field — still works
  for `babbage-002` / `davinci-002` base models, but on a deprecation
  path. Don't build around this long-term.

### Chat-only (no native FIM)
- **Anthropic Claude**: chat API. No native FIM. Workaround: system
  prompt instructing FIM behavior (`"Complete the code between
  <PREFIX> and <SUFFIX>, output only the middle, no commentary"`).
  Latency ~500 ms+, less reliable than native FIM, but the models are
  stronger overall.
- **OpenAI chat models** (gpt-4 / gpt-5 / o-series): same pattern.
- **Google Gemini**: same pattern.

### Backend layout
- `LlmGhostOpenAIBackend` — covers OpenAI proper plus OpenAI-compat
  servers (LM Studio, vLLM). One backend, base-URL configurable.
- `LlmGhostMistralBackend` — separate because the FIM endpoint shape
  differs.
- `LlmGhostClaudeBackend` — separate because the chat-FIM strategy
  needs prompt construction logic that would dirty the OpenAI backend.

### Settings layer (landed)

Configuration is a human-editable JSON file at
`~/.config/llmghost/settings.json` (XDG; `$XDG_CONFIG_HOME` honored), parsed
with json-glib and watched with `GFileMonitor` — edit it in gedit and
completions reload live, no restart. It is auto-created with a populated
default on first run. Every string value supports `${ENV_VAR}` interpolation
(unset → "" + a logged warning), so secrets can stay in the environment:
`"api_key": "${OPENAI_API_KEY}"`. A malformed file falls back to built-in
defaults without overwriting the user's file; a malformed *live edit* keeps the
last-good config.

Schema:

```jsonc
{
  "_help": "…ignored; _-prefixed keys are pseudo-comments",
  "backend": "ollama",          // active backend: ollama | openai | mistral
  "debounce_ms": 80,            // optional controller debounce override
  "backends": {
    "ollama":  { "host": "spark-2448", "port": 11434,
                 "model": "qwen3-coder-next:latest", "tokens": "Qwen" },
    "openai":  { "base_url": "https://api.openai.com/v1", "model": "gpt-4o-mini",
                 "api_key": "${OPENAI_API_KEY}", "mode": "chat" },
    "mistral": { "base_url": "https://codestral.mistral.ai/v1",
                 "model": "codestral-latest", "api_key": "${MISTRAL_API_KEY}" }
  }
}
```

Only the active backend is built (`llm_ghost_backend_new_from_settings()` in
`lib/llmghost-backend-factory.c`); the others are inert pre-filled config you
switch to by editing `backend`. Unknown keys are ignored, leaving room for a
future `"generic"` stanza. The plugin's Preferences button opens this file in
the editor.

### Remaining architectural prerequisites
1. **Secret storage**: API keys via libsecret (`gnome-keyring`). NEVER
   plaintext in config or env vars committed to scripts.
2. **Streaming (optional)**: cloud APIs return tokens over SSE.
   Streaming into the overlay drops perceived latency dramatically
   (~500 ms total → ~100 ms first-token visible). Add a
   `partial-data` signal to `LlmGhostBackend`; non-streaming backends
   ignore it. Implement only when a cloud backend is in and the
   non-streaming experience is provably bad — premature for Ollama at
   100 ms.

### Suggested order
1. `LlmGhostOpenAIBackend` first — tests "auth + base-URL + model
   selection" mechanics on familiar territory (LM Studio locally is
   free to iterate against). ✓ landed 2026-06-04
2. Settings layer — JSON config + live reload + Prefs button. ✓ landed 2026-06-05
3. `LlmGhostMistralBackend` — small variation on (1), exercises FIM-
   over-cloud. ✓ landed 2026-06-04
4. `LlmGhostClaudeBackend` — the awkward chat-FIM case.
5. Streaming as a cross-cutting concern after backends are stable.

When the second HTTP-based backend lands (so #1 above), extract
`lib/llmghost-http-util.{h,c}` from the Ollama backend's libsoup
plumbing — but only with both call sites in front of you to validate
the abstraction shape. Don't extract helpers speculatively. The
architecture already accommodates non-HTTP backends (Supermaven), so
the helper is for HTTP backends specifically, not "all backends".

## Phase 7 — Supermaven-style mid-line ghost rendering (deferred)

Currently we suppress ghost when the rest of the line has non-whitespace
text (`cursor_safe_for_ghost`). Real Supermaven shows short suggestions
even in that case — pushes existing text rightward or only suggests
tokens that fit. Real work, requires custom snapshot drawing or
text-widget manipulation that GTK 3 doesn't make easy. Deepest phase,
lowest priority until backends are fleshed out.

## Build cheat sheet

```
meson setup build              # first time
meson compile -C build         # build
./build/tests/llmghost-demo    # run demo
```

## Tests

`g_test` suite under `tests/`, wired into `meson test`:

- `--suite unit` — display-free: FIM tokens, the Ollama request-body builder
  (via `lib/llmghost-ollama-backend-internal.h`), the fake-backend async
  contract, and the instrumented mock backend.
- `--suite gui` — headless `LlmGhostController` tests under Xvfb (behaviour +
  a sanity-coordinate guard for the Phase-3 buffer-vs-window bug). Registered
  only when `xvfb-run` is found; it is the default test setup, so plain
  `meson test` runs everything.

```
meson test -C build              # all suites (gui wrapped in xvfb-run -a)
meson test -C build --suite unit # display-free subset
meson test -C build controller -v
```

The mock backend (`tests/mock-backend.{h,c}`) is deferred-by-default so tests
drive completion timing; the production `LlmGhostFakeBackend` can't, since it
completes synchronously and ignores its `GCancellable`.

Env-var overrides for ad-hoc model swaps:

```
# Same family (Qwen tokens, ~3× faster than the qwen3 default):
LLMGHOST_OLLAMA_MODEL=qwen2.5-coder:7b-base ./build/tests/llmghost-demo

# Different family — must also switch token set:
LLMGHOST_OLLAMA_MODEL=deepseek-coder:6.7b-base \
LLMGHOST_OLLAMA_TOKENS=DeepSeek ./build/tests/llmghost-demo

LLMGHOST_OLLAMA_MODEL=starcoder2:3b \
LLMGHOST_OLLAMA_TOKENS=StarCoder ./build/tests/llmghost-demo
```

For LSP support (clangd):

```
ln -sf build/compile_commands.json compile_commands.json
```

System deps (Ubuntu 24.04):

```
sudo apt install meson ninja-build libgtk-3-dev libglib2.0-dev \
                 libsoup-3.0-dev libjson-glib-dev \
                 libgedit-gtksourceview-dev libpeas-dev
```
