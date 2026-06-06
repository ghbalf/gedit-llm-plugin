# Secret Storage (libsecret) Design

**Status:** approved (brainstorm) — pending implementation plan.

## Goal

Let users keep LLM API keys in the system keyring (gnome-keyring via libsecret)
instead of plaintext in `settings.json` or environment variables. A config
string references a stored key with `${secret:NAME}`, resolved to the real value
at settings load-time; keys are entered and managed from the plugin's
Preferences dialog. This closes the standing "NEVER plaintext" gap (NOTES.md,
"Remaining architectural prerequisites" #1).

## Prerequisite (build environment)

The development machine does **not** currently have `libsecret-1` installed
(`pkg-config --exists libsecret-1` → false; `secret-tool` absent;
`dbus-run-session` present). The feature is a **hard build dependency** on
`libsecret-1`, so the dev package must be installed before the code can compile
or tests can run, e.g. on Debian/Ubuntu:

```
sudo apt install libsecret-1-dev
```

This is also why the real-keyring round-trip test is best-effort/skippable (see
Testing): a running secret service is not guaranteed in headless/SSH/CI.

## Background — how keys flow today

1. `settings.json` holds string values like `"api_key": "${OPENAI_API_KEY}"`.
2. At load, `LlmGhostSettings` parses the file and `_llm_ghost_settings_interpolate()`
   replaces every `${VAR}` with `g_getenv("VAR")` (unset → `""` + a warning).
   `interpolate_node()` walks objects and arrays.
3. The backend factory reads the interpolated params (`param_string(p, "api_key")`,
   the generic backend's `headers`/`url`, …) and hands the resolved strings to
   the backends.
4. The plugin (`plugin/llmghost-plugin.c`) owns the live `LlmGhostSettings` and
   reloads it via `GFileMonitor` when the file changes, emitting `"changed"`.
5. The Preferences entry point (`plugin/llmghost-configurable.c`,
   a `PeasGtkConfigurable`) is a **separate object** from the plugin — today it
   only shows the settings-file path and an "Open settings.json" button. It does
   not hold the plugin's live settings instance.

The secret feature plugs into step 2 (read) and step 5 (write UI), reusing the
existing interpolation and live-reload machinery.

## Architecture — pure core, thin libsecret edge

libsecret needs a running secret service (D-Bus + gnome-keyring) that may be
absent in headless/CI runs. To keep the logic deterministically testable, the
design separates an always-testable core from a thin, skippable libsecret edge:

```
settings.json string  ──(load-time interpolation)──►  resolved value
   "${secret:openai}"        │
                             ├─ "secret:" prefix ──► secret-source lookup ──► libsecret (real)
                             └─ "${OTHER}"       ──► g_getenv  (unchanged)        ▲
                                                                                  │
                                          tests inject a FAKE lookup here ────────┘
```

The interpolation reaches libsecret through a **module-level lookup function
pointer** (defaulting to the real libsecret call). Tests install a fake lookup,
so the parsing + fallback paths get full coverage with no daemon required.

## Components

### 1. `lib/llmghost-secret-store.{c,h}` — libsecret wrapper

A small, dependency-isolated module wrapping libsecret with a fixed schema.

- **Schema:** a `SecretSchema` named `de.mickautsch.llmghost`, one string
  attribute `name`; per-item label `llmghost: <name>`.
- **API (synchronous — settings load is synchronous):**
  ```c
  /* Look up the secret stored under @name. Returns a newly-allocated value, or
   * NULL if not found / on error (see *error). Caller frees with secret_password_free
   * via the wrapper, or g_free — wrapper normalizes to g_free. */
  char     *llm_ghost_secret_lookup (const char *name, GError **error);

  /* Store/overwrite @value under @name in the default collection. */
  gboolean  llm_ghost_secret_store  (const char *name, const char *value, GError **error);

  /* Remove the secret stored under @name. Missing key is success (idempotent). */
  gboolean  llm_ghost_secret_clear  (const char *name, GError **error);
  ```
  Implemented with `secret_password_lookup_sync` / `secret_password_store_sync`
  (collection `SECRET_COLLECTION_DEFAULT`) / `secret_password_clear_sync`.
- Note: a sync lookup can block to unlock a locked keyring (a desktop prompt).
  Acceptable for an interactive editor plugin.

### 2. Settings interpolation — the `${secret:NAME}` branch + test seam

In `lib/llmghost-settings.c`, extend `_llm_ghost_settings_interpolate()`:

- When the captured `${...}` body starts with the prefix `secret:`, the
  remainder is the secret NAME; resolve it via the **secret-source indirection**
  (below) instead of `g_getenv`.
- Everything without the `secret:` prefix stays an environment-variable lookup —
  `${ENV}` behavior is byte-for-byte unchanged.
- **Indirection / test seam:** a module-static function pointer
  `secret_lookup_fn(name) -> char*`, defaulting to a thin wrapper over
  `llm_ghost_secret_lookup`. A testing-only setter (declared in
  `llmghost-settings-internal.h`):
  ```c
  typedef char *(*LlmGhostSecretLookupFn) (const char *name);
  void _llm_ghost_settings_set_secret_lookup_for_testing (LlmGhostSecretLookupFn fn); /* NULL → restore default */
  ```

### 3. `collect_secret_refs()` — pure scan for the dialog

A pure function (behind `llmghost-settings-internal.h`) that walks an
(un-interpolated or interpolated) settings tree and returns the set of distinct
NAMEs referenced as `${secret:NAME}`:

```c
/* Returns a newly-allocated, de-duplicated, NULL-terminated array of secret
 * NAMEs referenced via ${secret:NAME} anywhere in @root's string values
 * (objects + arrays, recursively). Free with g_strfreev. */
char **_llm_ghost_settings_collect_secret_refs (JsonObject *root);
```

This drives the dialog's field list, so it offers exactly the secrets the active
config references — nothing hardcoded per backend. Secret refs must be
discoverable *before* interpolation replaces them; since the on-disk file is raw
(un-interpolated), the dialog parses the file from disk into a `JsonObject` and
passes that to this pure function. The live `LlmGhostSettings` instance is not
used for the scan (its tree is already interpolated). Fully unit-testable.

### 4. Preferences dialog — secrets section

Extend `plugin/llmghost-configurable.c`'s `configurable_create_widget`:

- Below the existing path label + "Open settings.json" button, add a **Secrets**
  section.
- Read the active settings file, run `_llm_ghost_settings_collect_secret_refs`,
  and for each NAME render a row: a label (`NAME`), a masked `GtkEntry`
  (`visibility = FALSE`) showing a placeholder when a value is already stored
  (probed via `llm_ghost_secret_lookup` returning non-NULL — the value is not
  displayed), and **Store** + **Clear** buttons calling
  `llm_ghost_secret_store` / `llm_ghost_secret_clear`.
- If the config references no secrets, show a one-line hint explaining the
  `${secret:NAME}` syntax instead of an empty section.
- Keep the dialog logic thin: every non-trivial decision (which names, whether a
  store/clear succeeded) is delegated to unit-tested functions; only widget
  construction lives here.

### 5. Live re-interpolation after a store

Storing/clearing a secret changes the keyring but not the file, so the plugin's
`GFileMonitor` would not fire. After a successful Store/Clear, the dialog
**touches `settings.json`** (bumping mtime, or rewriting identical bytes if an
mtime-only touch proves insufficient to raise `CHANGES_DONE_HINT`). That fires
the plugin's existing live-reload, which re-interpolates every value — picking up
the new keyring secret — with no restart and no new cross-object wiring. The
exact touch mechanism is validated by a test in the plan.

## Data flow

- **Read:** plugin starts → `LlmGhostSettings` loads → interpolation sees
  `${secret:openai}` → `secret_lookup_fn("openai")` → libsecret →
  `"sk-…"` baked into the params → factory hands it to the backend.
- **Write:** user opens Preferences → dialog lists `openai` (referenced by the
  config) → user types the key, clicks Store → `llm_ghost_secret_store("openai", …)`
  → keyring → dialog touches `settings.json` → plugin reloads → backend rebuilt
  with the resolved key.

## Error handling / fallback

- Any `${secret:NAME}` resolution failure — not found, keyring locked/declined,
  libsecret error — **logs a warning and substitutes `""`**, exactly mirroring
  the existing `${ENV}`-unset path. A missing key never crashes completions; the
  request fails cleanly downstream (empty key) as it already does.
- `store`/`clear` surface a `GError`; the dialog shows the message inline (e.g.
  a transient label) and does not touch the file on failure.
- `clear` of an absent key is success (idempotent).

## Dependency & build

- Add `dependency('libsecret-1')` to the meson deps used by `llmghost_lib` (and
  thus the plugin). Hard dependency — no optional feature flag (YAGNI); the
  feature is meaningless without it.
- `llmghost-secret-store.h` is internal (test-only seam), NOT added to installed
  `llmghost_headers`, consistent with `llmghost-http-util.h` /
  `llmghost-text-util.h` / `*-internal.h`.

## Testing

- **Interpolation `${secret:NAME}`** (`tests/test-settings.c`): install a fake
  lookup via `_llm_ghost_settings_set_secret_lookup_for_testing`; assert
  resolution, the not-found→`""`+warning path (using the established
  `g_test_expect_message` convention), `${ENV}` left untouched, and a
  `${secret:NAME}` nested inside an array (the generic-template shape).
- **`collect_secret_refs`** (`tests/test-settings.c`): de-dup, objects + arrays,
  mixed `${secret:}`/`${ENV}`, none-present.
- **Live re-interpolation touch**: in the settings reload harness, assert that
  the touch mechanism raises a reload (`"changed"`) and re-interpolation runs.
- **`llmghost-secret-store` round-trip** (new `tests/test-secret-store.c`):
  store → lookup → clear → lookup-returns-NULL, using a test-only NAME prefix
  and cleaning up after. Guarded by secret-service availability — if
  `secret_password_lookup_sync` of a probe fails because no service is running,
  `g_test_skip` with a clear message (mirrors the warning-convention CI spirit).
  Run under `dbus-run-session` when a service is available.
- **Untested surface:** the GTK dialog widget construction itself, consistent
  with the existing "Open settings.json" button and the no-GUI-over-SSH
  constraint. All logic it calls is unit-tested.

## File structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-secret-store.{c,h}` (new) | libsecret wrapper: schema + lookup/store/clear. Internal header. |
| `lib/llmghost-settings.c` (modify) | `${secret:NAME}` interpolation branch + secret-source indirection. |
| `lib/llmghost-settings-internal.h` (new or extend) | test-only seam: secret-lookup setter + `collect_secret_refs` decl. |
| `plugin/llmghost-configurable.c` (modify) | Secrets section in the prefs dialog + post-store file touch. |
| `tests/test-settings.c` (modify) | `${secret:}` interpolation + `collect_secret_refs` + reload-touch tests. |
| `tests/test-secret-store.c` (new) | libsecret round-trip, service-availability-guarded. |
| `lib/meson.build`, `tests/meson.build` (modify) | `libsecret-1` dep + new source/test wiring. |
| `NOTES.md` (modify) | Document the secret-storage feature; update prerequisite #1. |

## Out of scope / deferred

- Per-field UI beyond masked entry + Store/Clear (no strength meters, no reveal
  toggle) — YAGNI.
- Non-default keyring collections / custom unlock flows.
- Async libsecret (sync is sufficient at load and on a button click).
- SSE streaming and multi-line ghost rendering (separate roadmap items).
