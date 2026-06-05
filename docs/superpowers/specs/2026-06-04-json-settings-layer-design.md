# JSON Settings Layer — Design Spec

**Date:** 2026-06-04
**Phase:** 6 prerequisite (configuration layer).
**Status:** Draft for review (brainstorm decisions: v1 = full tier; secrets = plaintext + `${ENV}` interpolation). Awaiting user review before the plan.

## Goal

Replace env-var-only / hardcoded backend configuration with a **human-editable
JSON settings file** that the gedit plugin reads to choose and configure its
completion backend. The file lives at `~/.config/llmghost/settings.json`
(XDG-based), is parsed with json-glib, supports `${ENV_VAR}` interpolation in
string values, is auto-created with a populated example on first run, and is
**live-reloaded** (edit it in gedit, completions update without restarting
gedit). A minimal Preferences entry point opens the file in the editor.

This is the deferred Phase-6 prerequisite, chosen over GSettings/dconf because
this is a gedit plugin — users edit the config in gedit itself, with no
schema-compile or `dconf-editor` step. It is also the foundation the future
**config-driven generic backend** will build on (its request templates are
file-shaped, not env-var-shaped).

## v1 scope (chosen)

Full tier: loader + factory + auto-written default + live reload + Prefs entry
point. Missing/invalid config never crashes gedit — it falls back to built-in
defaults (and to the last-good config on a live-reload parse error).

## Architecture

Three units, two of them pure-logic and unit-testable, one thin GUI glue:

| Unit | Responsibility |
|------|----------------|
| `LlmGhostSettings` (`lib/llmghost-settings.{h,c}`) | Load/parse the JSON file, interpolate `${ENV}`, expose accessors, auto-write the default, watch the file (`GFileMonitor`) and emit `changed` on reload. |
| backend factory (`lib/llmghost-backend-factory.{h,c}`) | `llm_ghost_backend_new_from_settings(settings)` → build the active `LlmGhostBackend`. The one place that knows all backend types. |
| plugin integration (`plugin/llmghost-plugin.c`) | Own one `LlmGhostSettings`; build the backend via the factory; rebuild + reattach on `changed`; expose a `PeasGtkConfigurable` that opens the file. |

The parse + interpolate + factory logic is display-free and exhaustively
unit-tested. The `GFileMonitor` wiring and the Prefs widget are thin glue,
compile-checked and (necessarily) manually verified in gedit — see Testing.

## Config schema (v1)

```jsonc
{
  "_help": "Edit this file in gedit; completions reload automatically. ${VARS} expand from the environment.",
  "backend": "ollama",
  "debounce_ms": 80,
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

- `backend` — the active backend key. Unknown value → fall back to `ollama`
  defaults + a logged warning.
- `debounce_ms` — optional controller debounce override; absent → controller default.
- `backends` — a map of per-backend param objects. Only the active one is built;
  the rest are inert pre-filled config the user can switch to by editing
  `backend`. Each backend's params map 1:1 to its existing constructor args.
- Unknown keys are ignored (forward-compat: the future generic backend just adds
  a `"generic": { url, headers, request_template, response_path }` entry and a
  factory branch).
- json-glib parses strict JSON (no comments), so the auto-written default uses
  `_help`/`_`-prefixed string fields as pseudo-comments; `_`-prefixed keys are
  ignored by the loader.

## `${ENV_VAR}` interpolation

Applied to every JSON **string value** after parse: each `${NAME}` is replaced
with `g_getenv("NAME")`. Unset variable → empty string + a logged warning
(empty `api_key` → the backend simply sends no `Authorization` header, which is
already its NULL/"" behavior). Literal `$` not followed by `{` is left as-is. A
small pure helper `_llm_ghost_settings_interpolate(const char *in)` does this and
is unit-tested directly.

## Behavior

- **Load:** read the file at the configured path (default
  `g_get_user_config_dir()/llmghost/settings.json`). If absent → write the
  populated default above, then load it. Parse → interpolate → cache.
- **Accessors:** `active_backend()`, `debounce_ms()` (with a "set?" out-param or
  sentinel), and `backend_params(name)` returning the params `JsonObject` (or a
  typed getter per field — see plan). The factory reads these.
- **Live reload:** a `GFileMonitor` on the file; on `CHANGED`/`CREATED`, reload.
  If the new content parses, update the cache and emit `changed`. If it does
  **not** parse (mid-edit typo), keep the last-good cache and log — a working
  setup is never broken by an in-progress edit.
- **Factory:** switch on `active_backend()`; build via the matching
  `*_backend_new(...)` with interpolated params; unknown → `ollama` default.
- **Plugin:** on activate, create the settings object + build the backend; on the
  settings `changed` signal, rebuild the backend and re-attach it to the open
  views (the controllers already live per-view). On deactivate, drop the monitor.
- **Prefs:** implement `PeasGtkConfigurable` returning a small widget — a label
  showing the file path and an "Open settings.json" button that opens the file
  (via `gtk_show_uri_on_window` / a text/plain `GAppInfo`, which on a gedit
  desktop opens in gedit). No field-by-field widget UI in v1; the file *is* the UI.

## Error handling

- Missing file → auto-write default, use it.
- Unreadable / invalid JSON at startup → use built-in defaults, log a warning,
  do not write over the user's file.
- Invalid JSON on live reload → keep last-good config, log.
- `${VAR}` unset → "" + warning.
- Unknown `backend` value → `ollama` default + warning.
- The plugin must never crash gedit on any config error.

## Testing strategy

Automated tests are the correctness gate (the developer cannot run gedit/GUI).

**`test-settings` (unit, display-free):**
- `_llm_ghost_settings_interpolate`: plain, single `${VAR}` (set via `g_setenv`
  in-test), multiple vars, unset → "", literal `$` and `${` edge cases.
- Parse: given a JSON string (via a `*_load_from_string` test seam), assert
  `active_backend`, `debounce_ms`, and per-backend params; `_`-key ignored;
  unknown `backend` → fallback; malformed JSON → defaults.
- **Reload semantics:** drive the loader against a temp file — load, assert;
  rewrite the temp file + call the internal `reload()`; assert updated values and
  that `changed` fired; rewrite with broken JSON + `reload()`; assert last-good
  retained.
- **Factory:** for each `backend` value, `llm_ghost_backend_new_from_settings`
  returns a non-NULL `LlmGhostBackend` of the right GType
  (`LLM_GHOST_IS_OLLAMA_BACKEND` / `_OPENAI_BACKEND` / `_MISTRAL_BACKEND`).

**Not automatically verifiable (compile-checked + manual):** the `GFileMonitor`
async wiring (the `reload()` it calls is tested directly), the plugin's
rebuild-on-change integration, and the Prefs "open file" button — these are GUI/
libpeas glue that only a live gedit exercises. They are kept deliberately thin.
This limitation is inherent to the plugin layer and is called out, not hidden.

## File structure / build

- New: `lib/llmghost-settings.{h,c}` (+ `-internal.h` for the pure
  parse/interpolate/reload seam), `lib/llmghost-backend-factory.{h,c}`,
  `tests/test-settings.c`.
- Modify: `plugin/llmghost-plugin.c` (consume settings + factory; `changed`
  rebuild; `PeasGtkConfigurable`), `lib/llmghost.h` (umbrella), `lib/meson.build`
  (sources + installed headers: `llmghost-settings.h`,
  `llmghost-backend-factory.h`; not the `-internal.h`), `tests/meson.build`
  (register `test-settings`, unit).
- The factory links against all three backends — it is the single point of
  coupling, by design.

## Non-goals (deferred)

- libsecret secret storage (env interpolation is the interim).
- A field-by-field Preferences widget (the file is the UI in v1).
- The generic/template backend (separate later feature; the schema's
  ignore-unknown-keys rule leaves room for its `"generic"` stanza).
- Per-window / per-language settings; only one global config in v1.

## Self-review

- **Placeholders:** none — schema, interpolation rule, reload semantics, factory
  mapping, and every test case are concrete.
- **Consistency:** the schema's per-backend params match each backend's existing
  constructor args (ollama host/port/model/tokens; openai base/model/key/mode;
  mistral base/model/key); the factory builds exactly those.
- **Scope:** one cohesive subsystem (configuration) → one plan, ~6 tasks
  (settings loader + interpolation; factory; auto-write + live reload; plugin
  integration; Prefs entry point; docs). The GUI-glue parts are minimized and
  flagged as manually-verified.
- **Ambiguity:** interpolation on string values only; unset var → ""; last-good
  retained on reload parse error; `_`-keys ignored; unknown backend → ollama —
  all stated explicitly.
