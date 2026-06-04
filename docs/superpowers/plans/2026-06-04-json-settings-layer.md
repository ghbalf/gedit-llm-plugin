# JSON Settings Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hardcoded Ollama backend with a human-editable JSON settings file (`~/.config/llmghost/settings.json`) that selects and configures the completion backend, supports `${ENV_VAR}` interpolation, live-reloads on edit, and exposes an "Open settings.json" Preferences button.

**Architecture:** A new `LlmGhostSettings` GObject loads/parses/interpolates the JSON and watches it with a `GFileMonitor`, emitting `changed` on a successful live reload (keeping the last-good config on a parse error). A `llm_ghost_backend_new_from_settings()` factory is the single point that knows all backend GTypes. The plugin owns one settings object, builds the backend through the factory, and rebuilds + reattaches its per-view controllers on `changed`. A `PeasGtkConfigurable` extension supplies the Prefs button.

**Tech Stack:** C (gnu11), GLib/GObject, GIO (`GFileMonitor`), json-glib, GTK 3, libpeas-1.0 + libpeas-gtk-1.0, meson/ninja, GLib testing (`g_test`).

**Spec:** `docs/superpowers/specs/2026-06-04-json-settings-layer-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-settings.h` (new) | Public `LlmGhostSettings` type, `_new(path)`, accessors, `changed` signal, `default_path()`. |
| `lib/llmghost-settings-internal.h` (new) | Non-installed test seam: pure `_interpolate`, `_new_from_string`, `_reload`. |
| `lib/llmghost-settings.c` (new) | Parse + `${ENV}` interpolation + accessors + auto-write default + `GFileMonitor` reload. |
| `lib/llmghost-backend-factory.h` (new) | `llm_ghost_backend_new_from_settings(settings)`. |
| `lib/llmghost-backend-factory.c` (new) | Switch on active backend; build via the matching `*_backend_new()`. |
| `tests/test-settings.c` (new) | Unit tests: interpolate, parse/accessors, reload, factory GTypes. |
| `plugin/llmghost-plugin.c` (modify) | Own settings + factory; rebuild/reattach on `changed`; register the configurable. |
| `plugin/llmghost-configurable.{h,c}` (new) | `PeasGtkConfigurable` Prefs "Open settings.json" button. |
| `lib/llmghost.h`, `lib/meson.build`, `tests/meson.build`, `plugin/meson.build`, `meson.build` (modify) | Wire sources, headers, deps, tests. |

**Conventions to follow (verified in the existing tree):**
- Pure helpers behind `*-internal.h`, never added to `llmghost_headers` (the installed set).
- `G_DECLARE_FINAL_TYPE` + `G_DEFINE_TYPE` for plain GObjects; `G_IMPLEMENT_INTERFACE` for backends.
- `json_object_ref(...)` the parsed root, then `g_object_unref(parser)` — the cache outlives the parser.
- Unit tests are display-free and live in the `unit` suite; only the controller test needs Xvfb (`gui` suite). The settings tests stay in `unit`.

---

## Task 1: Settings file scaffold + `${ENV}` interpolation

Establishes the new source file, its test binary, and the one genuinely tricky pure function (env interpolation), fully TDD.

**Files:**
- Create: `lib/llmghost-settings-internal.h`
- Create: `lib/llmghost-settings.c`
- Create: `tests/test-settings.c`
- Modify: `lib/meson.build` (add `llmghost-settings.c` to `llmghost_sources`)
- Modify: `tests/meson.build` (register `test-settings`, `unit` suite)

- [ ] **Step 1: Create the internal header with only the interpolation declaration**

`lib/llmghost-settings-internal.h`:

```c
#pragma once

/* Testing-only internal API for LlmGhostSettings. NOT installed.
 * Grows across the plan; Task 1 declares only the pure interpolation helper. */

#include <glib.h>

G_BEGIN_DECLS

/* Replace each ${NAME} in @in with g_getenv("NAME"). An unset variable expands
 * to "" and logs a warning. A literal '$' not followed by '{', or a "${" with
 * no closing '}', is copied verbatim. @in may be NULL (→ ""). Newly-allocated. */
char *_llm_ghost_settings_interpolate (const char *in);

G_END_DECLS
```

- [ ] **Step 2: Create `tests/test-settings.c` with failing interpolation tests**

`tests/test-settings.c`:

```c
#include <glib.h>
#include "llmghost-settings-internal.h"

static void
test_interpolate_plain (void)
{
  char *r = _llm_ghost_settings_interpolate ("no vars here");
  g_assert_cmpstr (r, ==, "no vars here");
  g_free (r);
}

static void
test_interpolate_single (void)
{
  g_setenv ("LLMGHOST_TEST_VAR", "value", TRUE);
  char *r = _llm_ghost_settings_interpolate ("a-${LLMGHOST_TEST_VAR}-b");
  g_assert_cmpstr (r, ==, "a-value-b");
  g_free (r);
  g_unsetenv ("LLMGHOST_TEST_VAR");
}

static void
test_interpolate_multiple (void)
{
  g_setenv ("LLMGHOST_TEST_X", "1", TRUE);
  g_setenv ("LLMGHOST_TEST_Y", "2", TRUE);
  char *r = _llm_ghost_settings_interpolate ("${LLMGHOST_TEST_X}.${LLMGHOST_TEST_Y}");
  g_assert_cmpstr (r, ==, "1.2");
  g_free (r);
  g_unsetenv ("LLMGHOST_TEST_X");
  g_unsetenv ("LLMGHOST_TEST_Y");
}

static void
test_interpolate_unset (void)
{
  g_unsetenv ("LLMGHOST_TEST_NOPE");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  char *r = _llm_ghost_settings_interpolate ("x${LLMGHOST_TEST_NOPE}y");
  g_assert_cmpstr (r, ==, "xy");
  g_free (r);
  g_test_assert_expected_messages ();
}

static void
test_interpolate_literal_dollar (void)
{
  /* A '$' not starting a "${...}" is preserved, and an unterminated "${" too. */
  char *a = _llm_ghost_settings_interpolate ("cost is $5");
  g_assert_cmpstr (a, ==, "cost is $5");
  g_free (a);
  char *b = _llm_ghost_settings_interpolate ("broken ${UNCLOSED");
  g_assert_cmpstr (b, ==, "broken ${UNCLOSED");
  g_free (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/settings/interpolate/plain",          test_interpolate_plain);
  g_test_add_func ("/settings/interpolate/single",         test_interpolate_single);
  g_test_add_func ("/settings/interpolate/multiple",       test_interpolate_multiple);
  g_test_add_func ("/settings/interpolate/unset",          test_interpolate_unset);
  g_test_add_func ("/settings/interpolate/literal-dollar", test_interpolate_literal_dollar);
  return g_test_run ();
}
```

- [ ] **Step 3: Create `lib/llmghost-settings.c` with only the interpolation function**

`lib/llmghost-settings.c`:

```c
#define G_LOG_DOMAIN "llmghost-settings"

#include "llmghost-settings-internal.h"

#include <string.h>

char *
_llm_ghost_settings_interpolate (const char *in)
{
  if (in == NULL)
    return g_strdup ("");

  GString *out = g_string_new (NULL);
  const char *p = in;
  while (*p != '\0')
    {
      if (p[0] == '$' && p[1] == '{')
        {
          const char *end = strchr (p + 2, '}');
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              const char *val = g_getenv (name);
              if (val == NULL)
                {
                  g_warning ("environment variable ${%s} is not set; using \"\"", name);
                  val = "";
                }
              g_string_append (out, val);
              g_free (name);
              p = end + 1;
              continue;
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}
```

- [ ] **Step 4: Wire the new source and test into meson**

In `lib/meson.build`, add `'llmghost-settings.c',` to the `llmghost_sources = files(...)` list (after `'llmghost-overlay.c',` is fine — order is cosmetic). Do **not** add any header to `llmghost_headers` yet.

In `tests/meson.build`, append after the `test-mistral-body` block:

```meson
test_settings = executable(
  'test-settings',
  'test-settings.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('settings', test_settings, suite: 'unit')
```

- [ ] **Step 5: Configure, build, and run — verify the interpolation tests pass**

Run:
```bash
meson setup build 2>/dev/null || meson setup --reconfigure build
ninja -C build
meson test -C build --suite unit settings -v
```
Expected: `test-settings` builds and all 5 `/settings/interpolate/*` tests PASS.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-settings-internal.h lib/llmghost-settings.c tests/test-settings.c lib/meson.build tests/meson.build
git commit -m "feat(settings): add settings source scaffold and \${ENV} interpolation"
```

---

## Task 2: Settings GObject — parse, interpolate tree, accessors

Adds the public type, the string-only test constructor, JSON parsing with whole-tree interpolation, the built-in defaults, and the read accessors the factory will use.

**Files:**
- Create: `lib/llmghost-settings.h`
- Modify: `lib/llmghost-settings-internal.h` (add `_new_from_string`)
- Modify: `lib/llmghost-settings.c` (add the GObject + parse + accessors)
- Modify: `lib/llmghost.h` (umbrella include)
- Modify: `lib/meson.build` (add `llmghost-settings.h` to `llmghost_headers`)
- Modify: `tests/test-settings.c` (add parse/accessor tests)

- [ ] **Step 1: Create the public header `lib/llmghost-settings.h`**

```c
#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_SETTINGS (llm_ghost_settings_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostSettings, llm_ghost_settings,
                      LLM_GHOST, SETTINGS, GObject)

/**
 * llm_ghost_settings_new:
 * @path: settings.json path, or %NULL for the XDG default
 *        ($XDG_CONFIG_HOME/llmghost/settings.json).
 *
 * Loads the file (writing a populated default first if it is absent),
 * interpolates ${ENV_VAR} in every string value, and watches the file for
 * live edits. A malformed file falls back to built-in defaults and is left
 * untouched. Emits "changed" when a live edit reloads successfully.
 */
LlmGhostSettings *llm_ghost_settings_new (const char *path);

/* The XDG default settings path. Newly-allocated; free with g_free(). */
char *llm_ghost_settings_default_path (void);

/* Active backend key ("ollama"/"openai"/"mistral"). Never NULL; a missing,
 * empty, or non-string value yields "ollama". Owned by @self, valid until the
 * next reload. */
const char *llm_ghost_settings_get_active_backend (LlmGhostSettings *self);

/* Optional debounce override. Returns TRUE and writes *out_ms when the config
 * sets a positive integer "debounce_ms"; returns FALSE when absent. */
gboolean llm_ghost_settings_get_debounce_ms (LlmGhostSettings *self,
                                             guint            *out_ms);

/* The interpolated params object for backend @name (under "backends"), or
 * %NULL when absent. Owned by @self, valid until the next reload. */
JsonObject *llm_ghost_settings_get_backend_params (LlmGhostSettings *self,
                                                   const char       *name);

G_END_DECLS
```

- [ ] **Step 2: Extend the internal header with the string constructor**

In `lib/llmghost-settings-internal.h`, change the include line `#include <glib.h>` to also include the public header, and add the new declaration:

```c
#pragma once

/* Testing-only internal API for LlmGhostSettings. NOT installed. */

#include <glib.h>
#include "llmghost-settings.h"

G_BEGIN_DECLS

/* Replace each ${NAME} in @in with g_getenv("NAME"). An unset variable expands
 * to "" and logs a warning. A literal '$' not followed by '{', or a "${" with
 * no closing '}', is copied verbatim. @in may be NULL (→ ""). Newly-allocated. */
char *_llm_ghost_settings_interpolate (const char *in);

/* Build a settings object from a JSON string with no backing file and no
 * monitor (test seam). Malformed JSON → built-in defaults. */
LlmGhostSettings *_llm_ghost_settings_new_from_string (const char *json);

G_END_DECLS
```

- [ ] **Step 3: Add failing parse/accessor tests to `tests/test-settings.c`**

Add `#include "llmghost-settings.h"` and `#include <json-glib/json-glib.h>` to the includes, then add these functions and register them in `main`:

```c
static void
test_parse_active_backend (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"mistral\",\"backends\":{}}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "mistral");
  g_object_unref (s);
}

static void
test_parse_active_backend_default (void)
{
  /* Missing "backend" → "ollama". */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"backends\":{}}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "ollama");
  g_object_unref (s);
}

static void
test_parse_unknown_backend_passthrough (void)
{
  /* An unrecognised but well-formed string is returned verbatim; the factory,
   * not the accessor, maps unknown → ollama. */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"frobnicate\"}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "frobnicate");
  g_object_unref (s);
}

static void
test_parse_debounce (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"debounce_ms\":120}");
  guint ms = 0;
  g_assert_true (llm_ghost_settings_get_debounce_ms (s, &ms));
  g_assert_cmpuint (ms, ==, 120);
  g_object_unref (s);
}

static void
test_parse_debounce_absent (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{}");
  guint ms = 999;
  g_assert_false (llm_ghost_settings_get_debounce_ms (s, &ms));
  g_object_unref (s);
}

static void
test_parse_backend_params_interpolated (void)
{
  g_setenv ("LLMGHOST_TEST_KEY", "sk-xyz", TRUE);
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backends\":{\"openai\":{\"model\":\"gpt\",\"api_key\":\"${LLMGHOST_TEST_KEY}\"}}}");
  JsonObject *p = llm_ghost_settings_get_backend_params (s, "openai");
  g_assert_nonnull (p);
  g_assert_cmpstr (json_object_get_string_member (p, "model"),   ==, "gpt");
  g_assert_cmpstr (json_object_get_string_member (p, "api_key"), ==, "sk-xyz");
  g_object_unref (s);
  g_unsetenv ("LLMGHOST_TEST_KEY");
}

static void
test_parse_underscore_key_ignored (void)
{
  /* "_help" is present but never interpreted; parsing still succeeds. */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"_help\":\"hello\",\"backend\":\"openai\"}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "openai");
  g_object_unref (s);
}

static void
test_parse_malformed_uses_defaults (void)
{
  /* Built-in default backend is "ollama". */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("this is not json {");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "ollama");
  g_object_unref (s);
}
```

Register in `main` (before `return g_test_run ();`):

```c
  g_test_add_func ("/settings/parse/active-backend",         test_parse_active_backend);
  g_test_add_func ("/settings/parse/active-backend-default", test_parse_active_backend_default);
  g_test_add_func ("/settings/parse/unknown-passthrough",    test_parse_unknown_backend_passthrough);
  g_test_add_func ("/settings/parse/debounce",               test_parse_debounce);
  g_test_add_func ("/settings/parse/debounce-absent",        test_parse_debounce_absent);
  g_test_add_func ("/settings/parse/params-interpolated",    test_parse_backend_params_interpolated);
  g_test_add_func ("/settings/parse/underscore-ignored",     test_parse_underscore_key_ignored);
  g_test_add_func ("/settings/parse/malformed-defaults",     test_parse_malformed_uses_defaults);
```

- [ ] **Step 4: Run to verify the new tests FAIL to compile/link**

Run:
```bash
ninja -C build
```
Expected: FAIL — undefined reference to `_llm_ghost_settings_new_from_string`, `llm_ghost_settings_get_active_backend`, etc.

- [ ] **Step 5: Implement the GObject, parsing, and accessors in `lib/llmghost-settings.c`**

Replace the entire current contents of `lib/llmghost-settings.c` with the following (this keeps `_interpolate` and adds the rest). The file-loading parts (`llm_ghost_settings_new`, `_reload`, monitor) arrive in Task 3 — the `g_signal_new("changed", ...)` is registered now so Task 3 only emits it.

```c
#define G_LOG_DOMAIN "llmghost-settings"

#include "llmghost-settings.h"
#include "llmghost-settings-internal.h"

#include <string.h>

/* Built-in default config, written to disk on first run and used as the
 * fallback when the user's file is missing or malformed. */
static const char DEFAULT_SETTINGS_JSON[] =
  "{\n"
  "  \"_help\": \"Edit this file in gedit; completions reload automatically. ${VARS} expand from the environment.\",\n"
  "  \"backend\": \"ollama\",\n"
  "  \"debounce_ms\": 80,\n"
  "  \"backends\": {\n"
  "    \"ollama\":  { \"host\": \"spark-2448\", \"port\": 11434,\n"
  "                 \"model\": \"qwen3-coder-next:latest\", \"tokens\": \"Qwen\" },\n"
  "    \"openai\":  { \"base_url\": \"https://api.openai.com/v1\", \"model\": \"gpt-4o-mini\",\n"
  "                 \"api_key\": \"${OPENAI_API_KEY}\", \"mode\": \"chat\" },\n"
  "    \"mistral\": { \"base_url\": \"https://codestral.mistral.ai/v1\",\n"
  "                 \"model\": \"codestral-latest\", \"api_key\": \"${MISTRAL_API_KEY}\" }\n"
  "  }\n"
  "}\n";

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _LlmGhostSettings
{
  GObject       parent_instance;
  char         *path;        /* backing file, or NULL (string-only) */
  JsonObject   *root;        /* interpolated config (owned) */
  GFileMonitor *monitor;     /* NULL when no backing file */
};

G_DEFINE_TYPE (LlmGhostSettings, llm_ghost_settings, G_TYPE_OBJECT)

/* ---- ${ENV} interpolation ---------------------------------------------- */

char *
_llm_ghost_settings_interpolate (const char *in)
{
  if (in == NULL)
    return g_strdup ("");

  GString *out = g_string_new (NULL);
  const char *p = in;
  while (*p != '\0')
    {
      if (p[0] == '$' && p[1] == '{')
        {
          const char *end = strchr (p + 2, '}');
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              const char *val = g_getenv (name);
              if (val == NULL)
                {
                  g_warning ("environment variable ${%s} is not set; using \"\"", name);
                  val = "";
                }
              g_string_append (out, val);
              g_free (name);
              p = end + 1;
              continue;
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

/* Replace every string value in @obj (recursing into nested objects) with its
 * interpolation. Our config has no string arrays, so arrays are left alone. */
static void
interpolate_object (JsonObject *obj)
{
  GList *members = json_object_get_members (obj);   /* snapshot of keys */
  for (GList *l = members; l != NULL; l = l->next)
    {
      const char *key = l->data;
      JsonNode *child = json_object_get_member (obj, key);
      if (JSON_NODE_HOLDS_OBJECT (child))
        interpolate_object (json_node_get_object (child));
      else if (JSON_NODE_HOLDS_VALUE (child) &&
               json_node_get_value_type (child) == G_TYPE_STRING)
        {
          char *interp = _llm_ghost_settings_interpolate (json_node_get_string (child));
          json_object_set_string_member (obj, key, interp);
          g_free (interp);
        }
    }
  g_list_free (members);
}

/* ---- parsing ----------------------------------------------------------- */

/* Parse @json and interpolate every string. Returns an owned JsonObject, or
 * NULL when the text is not a JSON object. */
static JsonObject *
parse_and_interpolate (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  if (!json_parser_load_from_data (parser, json, -1, &error))
    {
      g_warning ("parse error: %s", error->message);
      g_clear_error (&error);
      g_object_unref (parser);
      return NULL;
    }

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_warning ("top-level value is not an object");
      g_object_unref (parser);
      return NULL;
    }

  JsonObject *obj = json_object_ref (json_node_get_object (root));
  g_object_unref (parser);                 /* cache outlives the parser */
  interpolate_object (obj);
  return obj;
}

/* Built-in defaults; always valid by construction. */
static JsonObject *
default_object (void)
{
  JsonObject *obj = parse_and_interpolate (DEFAULT_SETTINGS_JSON);
  g_assert (obj != NULL);
  return obj;
}

/* Swap in a new cached root, dropping the old one. Takes ownership of @obj. */
static void
set_root (LlmGhostSettings *self, JsonObject *obj)
{
  if (self->root != NULL)
    json_object_unref (self->root);
  self->root = obj;
}

/* ---- construction (string seam) ---------------------------------------- */

LlmGhostSettings *
_llm_ghost_settings_new_from_string (const char *json)
{
  LlmGhostSettings *self = g_object_new (LLM_GHOST_TYPE_SETTINGS, NULL);
  JsonObject *obj = parse_and_interpolate (json);
  set_root (self, obj != NULL ? obj : default_object ());
  return self;
}

char *
llm_ghost_settings_default_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "llmghost", "settings.json", NULL);
}

/* ---- accessors --------------------------------------------------------- */

const char *
llm_ghost_settings_get_active_backend (LlmGhostSettings *self)
{
  JsonNode *n = json_object_get_member (self->root, "backend");
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n) &&
      json_node_get_value_type (n) == G_TYPE_STRING)
    {
      const char *s = json_node_get_string (n);
      if (s != NULL && *s != '\0')
        return s;
    }
  return "ollama";
}

gboolean
llm_ghost_settings_get_debounce_ms (LlmGhostSettings *self, guint *out_ms)
{
  JsonNode *n = json_object_get_member (self->root, "debounce_ms");
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
    {
      GType t = json_node_get_value_type (n);
      if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE)
        {
          gint64 v = (t == G_TYPE_DOUBLE) ? (gint64) json_node_get_double (n)
                                          : json_node_get_int (n);
          if (v > 0)
            {
              if (out_ms != NULL)
                *out_ms = (guint) v;
              return TRUE;
            }
        }
    }
  return FALSE;
}

JsonObject *
llm_ghost_settings_get_backend_params (LlmGhostSettings *self, const char *name)
{
  JsonNode *bn = json_object_get_member (self->root, "backends");
  if (bn == NULL || !JSON_NODE_HOLDS_OBJECT (bn))
    return NULL;
  JsonObject *backends = json_node_get_object (bn);
  JsonNode *pn = json_object_get_member (backends, name);
  if (pn == NULL || !JSON_NODE_HOLDS_OBJECT (pn))
    return NULL;
  return json_node_get_object (pn);
}

/* ---- GObject lifecycle ------------------------------------------------- */

static void
llm_ghost_settings_finalize (GObject *object)
{
  LlmGhostSettings *self = LLM_GHOST_SETTINGS (object);
  if (self->monitor != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->monitor, self);
      g_clear_object (&self->monitor);
    }
  g_clear_pointer (&self->root, json_object_unref);
  g_clear_pointer (&self->path, g_free);
  G_OBJECT_CLASS (llm_ghost_settings_parent_class)->finalize (object);
}

static void
llm_ghost_settings_class_init (LlmGhostSettingsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_settings_finalize;

  /* Emitted after a live reload swaps in a new, valid config. */
  signals[SIG_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
llm_ghost_settings_init (LlmGhostSettings *self)
{
  (void) self;
}
```

- [ ] **Step 6: Add the public header to the umbrella and the installed headers**

In `lib/llmghost.h`, add after the existing includes:
```c
#include "llmghost-settings.h"
```

In `lib/meson.build`, add `'llmghost-settings.h',` to the `llmghost_headers = files(...)` list.

- [ ] **Step 7: Build and run — verify all settings tests pass**

Run:
```bash
ninja -C build
meson test -C build --suite unit settings -v
```
Expected: PASS — the 5 interpolation tests plus the 8 parse/accessor tests.

- [ ] **Step 8: Commit**

```bash
git add lib/llmghost-settings.h lib/llmghost-settings-internal.h lib/llmghost-settings.c lib/llmghost.h lib/meson.build tests/test-settings.c
git commit -m "feat(settings): add LlmGhostSettings type, JSON parse + accessors"
```

---

## Task 3: File loading, auto-written default, and live reload

Adds the real `llm_ghost_settings_new(path)`, first-run default file creation, the `GFileMonitor`, and the `_reload()` seam with last-good-on-error semantics.

**Files:**
- Modify: `lib/llmghost-settings-internal.h` (add `_reload`)
- Modify: `lib/llmghost-settings.c` (add file loading + monitor + reload)
- Modify: `tests/test-settings.c` (add auto-write/reload tests)

- [ ] **Step 1: Declare `_reload` in the internal header**

In `lib/llmghost-settings-internal.h`, add before `G_END_DECLS`:

```c
/* Re-read the backing file and refresh the cache (what the GFileMonitor
 * callback calls). On parse success: update cache + emit "changed". On parse
 * failure or read error: keep the last-good cache and log; no signal. No-op if
 * the object has no backing file. */
void _llm_ghost_settings_reload (LlmGhostSettings *self);
```

- [ ] **Step 2: Add failing file/reload tests to `tests/test-settings.c`**

Add `#include <gio/gio.h>` and `#include <glib/gstdio.h>` to the includes. Add a small helper and the tests, then register them:

```c
/* Returns a newly-allocated path to a fresh temp settings file seeded with
 * @contents. Caller frees the path; the file lives in a temp dir that the test
 * intentionally leaves behind (cheap, and avoids teardown races). */
static char *
write_temp_settings (const char *contents)
{
  GError *error = NULL;
  char *dir = g_dir_make_tmp ("llmghost-settings-XXXXXX", &error);
  g_assert_no_error (error);
  char *path = g_build_filename (dir, "settings.json", NULL);
  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
  g_free (dir);
  return path;
}

static void
test_autowrite_default (void)
{
  /* Point at a non-existent file inside a fresh temp dir. */
  GError *error = NULL;
  char *dir = g_dir_make_tmp ("llmghost-settings-XXXXXX", &error);
  g_assert_no_error (error);
  char *path = g_build_filename (dir, "settings.json", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  LlmGhostSettings *s = llm_ghost_settings_new (path);
  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));            /* written */
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "ollama");

  g_object_unref (s);
  g_free (path);
  g_free (dir);
}

static void
on_changed_count (LlmGhostSettings *s, gpointer data)
{
  (void) s;
  (*(int *) data)++;
}

static void
test_reload_updates_and_signals (void)
{
  char *path = write_temp_settings ("{\"backend\":\"mistral\"}");
  LlmGhostSettings *s = llm_ghost_settings_new (path);
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "mistral");

  int changed = 0;
  g_signal_connect (s, "changed", G_CALLBACK (on_changed_count), &changed);

  GError *error = NULL;
  g_assert_true (g_file_set_contents (path, "{\"backend\":\"openai\"}", -1, &error));
  g_assert_no_error (error);

  _llm_ghost_settings_reload (s);
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "openai");
  g_assert_cmpint (changed, ==, 1);

  g_object_unref (s);
  g_free (path);
}

static void
test_reload_broken_keeps_last_good (void)
{
  char *path = write_temp_settings ("{\"backend\":\"openai\"}");
  LlmGhostSettings *s = llm_ghost_settings_new (path);

  int changed = 0;
  g_signal_connect (s, "changed", G_CALLBACK (on_changed_count), &changed);

  GError *error = NULL;
  g_assert_true (g_file_set_contents (path, "broken {", -1, &error));
  g_assert_no_error (error);

  _llm_ghost_settings_reload (s);                                   /* emits a warning */
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "openai"); /* unchanged */
  g_assert_cmpint (changed, ==, 0);                                /* no signal */

  g_object_unref (s);
  g_free (path);
}
```

Register in `main`:
```c
  g_test_add_func ("/settings/file/autowrite-default",   test_autowrite_default);
  g_test_add_func ("/settings/file/reload-updates",      test_reload_updates_and_signals);
  g_test_add_func ("/settings/file/reload-broken",       test_reload_broken_keeps_last_good);
```

- [ ] **Step 3: Run to verify the new tests FAIL to link**

Run:
```bash
ninja -C build
```
Expected: FAIL — undefined reference to `llm_ghost_settings_new` and `_llm_ghost_settings_reload`.

- [ ] **Step 4: Implement file loading, the monitor, and reload in `lib/llmghost-settings.c`**

Add `#include <gio/gio.h>` near the top (after `#include <string.h>`). Then add these functions. Place the file-loading helpers just before the `/* ---- GObject lifecycle ---- */` section, and `_reload` with them:

```c
/* ---- file loading + live reload ---------------------------------------- */

static void
ensure_file_exists (const char *path)
{
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    return;
  char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_free (dir);

  GError *error = NULL;
  if (!g_file_set_contents (path, DEFAULT_SETTINGS_JSON, -1, &error))
    {
      g_warning ("could not write default %s: %s", path, error->message);
      g_clear_error (&error);
    }
}

/* Read self->path into the cache. On read or parse failure, fall back to
 * built-in defaults WITHOUT overwriting the user's file. */
static void
load_from_file (LlmGhostSettings *self)
{
  char *data = NULL;
  GError *error = NULL;
  if (!g_file_get_contents (self->path, &data, NULL, &error))
    {
      g_warning ("could not read %s: %s; using defaults", self->path, error->message);
      g_clear_error (&error);
      set_root (self, default_object ());
      return;
    }
  JsonObject *obj = parse_and_interpolate (data);
  g_free (data);
  set_root (self, obj != NULL ? obj : default_object ());
}

void
_llm_ghost_settings_reload (LlmGhostSettings *self)
{
  if (self->path == NULL)
    return;

  char *data = NULL;
  GError *error = NULL;
  if (!g_file_get_contents (self->path, &data, NULL, &error))
    {
      g_warning ("reload could not read %s: %s; keeping last config",
                 self->path, error->message);
      g_clear_error (&error);
      return;
    }
  JsonObject *obj = parse_and_interpolate (data);
  g_free (data);
  if (obj == NULL)
    {
      g_warning ("reload parse failed; keeping last config");
      return;                                    /* keep last-good, no signal */
    }
  set_root (self, obj);
  g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static void
on_file_changed (GFileMonitor *mon, GFile *file, GFile *other,
                 GFileMonitorEvent ev, gpointer user_data)
{
  (void) mon; (void) file; (void) other;
  if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      ev == G_FILE_MONITOR_EVENT_CREATED)
    _llm_ghost_settings_reload (LLM_GHOST_SETTINGS (user_data));
}

LlmGhostSettings *
llm_ghost_settings_new (const char *path)
{
  LlmGhostSettings *self = g_object_new (LLM_GHOST_TYPE_SETTINGS, NULL);
  self->path = path != NULL ? g_strdup (path) : llm_ghost_settings_default_path ();

  ensure_file_exists (self->path);
  load_from_file (self);

  GFile *file = g_file_new_for_path (self->path);
  GError *error = NULL;
  self->monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
  if (self->monitor != NULL)
    g_signal_connect (self->monitor, "changed", G_CALLBACK (on_file_changed), self);
  else
    {
      g_warning ("could not watch %s: %s", self->path, error->message);
      g_clear_error (&error);
    }
  g_object_unref (file);
  return self;
}
```

Note: the listener uses `CHANGES_DONE_HINT` (fired once an editor finishes writing) rather than every `CHANGED`, which de-bounces multi-write saves; `CREATED` covers atomic save-by-rename.

- [ ] **Step 5: Build and run — all settings tests pass**

Run:
```bash
ninja -C build
meson test -C build --suite unit settings -v
```
Expected: PASS — interpolation + parse + the 3 new file/reload tests. The `reload-broken` test prints expected `parse error` / `reload parse failed` warnings (non-fatal).

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-settings-internal.h lib/llmghost-settings.c tests/test-settings.c
git commit -m "feat(settings): file loading, auto-written default, live reload"
```

---

## Task 4: Backend factory

The single place that maps the active-backend key + params to a constructed `LlmGhostBackend`.

**Files:**
- Create: `lib/llmghost-backend-factory.h`
- Create: `lib/llmghost-backend-factory.c`
- Modify: `lib/llmghost.h` (umbrella include)
- Modify: `lib/meson.build` (source + header)
- Modify: `tests/test-settings.c` (factory GType tests)

- [ ] **Step 1: Create `lib/llmghost-backend-factory.h`**

```c
#pragma once

#include "llmghost-backend.h"
#include "llmghost-settings.h"

G_BEGIN_DECLS

/**
 * llm_ghost_backend_new_from_settings:
 * @settings: loaded settings.
 *
 * Builds the backend named by the active "backend" key, configured from its
 * "backends.<name>" params. An unknown name falls back to a default Ollama
 * backend. Returns a new reference the caller owns.
 */
LlmGhostBackend *llm_ghost_backend_new_from_settings (LlmGhostSettings *settings);

G_END_DECLS
```

- [ ] **Step 2: Add failing factory tests to `tests/test-settings.c`**

Add `#include "llmghost-backend-factory.h"` and `#include "llmghost-ollama-backend.h"`, `#include "llmghost-openai-backend.h"`, `#include "llmghost-mistral-backend.h"` to the includes. Add the tests and register them:

```c
static void
test_factory_ollama (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"ollama\","
    "\"backends\":{\"ollama\":{\"host\":\"h\",\"port\":1,\"model\":\"m\",\"tokens\":\"Qwen\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OLLAMA_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_openai (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"openai\","
    "\"backends\":{\"openai\":{\"base_url\":\"http://x/v1\",\"model\":\"m\",\"mode\":\"chat\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OPENAI_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_mistral (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"mistral\","
    "\"backends\":{\"mistral\":{\"base_url\":\"http://x/v1\",\"model\":\"m\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_MISTRAL_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_unknown_falls_back_to_ollama (void)
{
  g_test_expect_message ("llmghost-factory", G_LOG_LEVEL_WARNING, "*unknown backend*");
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"backend\":\"frobnicate\"}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OLLAMA_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
  g_test_assert_expected_messages ();
}

static void
test_factory_missing_params_ok (void)
{
  /* No "backends" object at all → factory still builds ollama defaults. */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"backend\":\"ollama\"}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OLLAMA_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}
```

Register in `main`:
```c
  g_test_add_func ("/settings/factory/ollama",        test_factory_ollama);
  g_test_add_func ("/settings/factory/openai",        test_factory_openai);
  g_test_add_func ("/settings/factory/mistral",       test_factory_mistral);
  g_test_add_func ("/settings/factory/unknown",       test_factory_unknown_falls_back_to_ollama);
  g_test_add_func ("/settings/factory/missing-params", test_factory_missing_params_ok);
```

- [ ] **Step 3: Run to verify the factory tests FAIL to link**

Run:
```bash
ninja -C build
```
Expected: FAIL — undefined reference to `llm_ghost_backend_new_from_settings`.

- [ ] **Step 4: Implement `lib/llmghost-backend-factory.c`**

```c
#define G_LOG_DOMAIN "llmghost-factory"

#include "llmghost-backend-factory.h"

#include "llmghost-ollama-backend.h"
#include "llmghost-openai-backend.h"
#include "llmghost-mistral-backend.h"

#include <json-glib/json-glib.h>

static const char *
param_string (JsonObject *p, const char *key)
{
  if (p == NULL || !json_object_has_member (p, key))
    return NULL;
  JsonNode *n = json_object_get_member (p, key);
  if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (n);
}

static gint64
param_int (JsonObject *p, const char *key, gint64 fallback)
{
  if (p == NULL || !json_object_has_member (p, key))
    return fallback;
  JsonNode *n = json_object_get_member (p, key);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return fallback;
  GType t = json_node_get_value_type (n);
  if (t == G_TYPE_INT64)  return json_node_get_int (n);
  if (t == G_TYPE_DOUBLE) return (gint64) json_node_get_double (n);
  return fallback;
}

static LlmGhostBackend *
build_ollama (JsonObject *p)
{
  LlmGhostBackend *b = llm_ghost_ollama_backend_new (param_string (p, "host"),
                                                     (guint16) param_int (p, "port", 0),
                                                     param_string (p, "model"));
  const char *tokens = param_string (p, "tokens");
  if (tokens != NULL && *tokens != '\0')
    {
      const LlmGhostFimTokens *t = llm_ghost_fim_tokens_lookup_builtin (tokens);
      if (t != NULL)
        llm_ghost_ollama_backend_set_fim_tokens (LLM_GHOST_OLLAMA_BACKEND (b), t);
      else
        g_warning ("unknown FIM token set \"%s\"; using default", tokens);
    }
  return b;
}

static LlmGhostBackend *
build_openai (JsonObject *p)
{
  const char *mode = param_string (p, "mode");
  LlmGhostOpenAIMode m =
    (mode != NULL && g_ascii_strcasecmp (mode, "completions") == 0)
      ? LLM_GHOST_OPENAI_MODE_COMPLETIONS
      : LLM_GHOST_OPENAI_MODE_CHAT;
  return llm_ghost_openai_backend_new (param_string (p, "base_url"),
                                       param_string (p, "model"),
                                       param_string (p, "api_key"),
                                       m);
}

static LlmGhostBackend *
build_mistral (JsonObject *p)
{
  return llm_ghost_mistral_backend_new (param_string (p, "base_url"),
                                        param_string (p, "model"),
                                        param_string (p, "api_key"));
}

LlmGhostBackend *
llm_ghost_backend_new_from_settings (LlmGhostSettings *settings)
{
  const char *which = llm_ghost_settings_get_active_backend (settings);
  JsonObject *p = llm_ghost_settings_get_backend_params (settings, which);

  if (g_strcmp0 (which, "openai") == 0)
    return build_openai (p);
  if (g_strcmp0 (which, "mistral") == 0)
    return build_mistral (p);
  if (g_strcmp0 (which, "ollama") != 0)
    g_warning ("unknown backend \"%s\"; using ollama", which);
  return build_ollama (p);
}
```

- [ ] **Step 5: Wire the factory into the build and umbrella header**

In `lib/meson.build`, add `'llmghost-backend-factory.c',` to `llmghost_sources` and `'llmghost-backend-factory.h',` to `llmghost_headers`.

In `lib/llmghost.h`, add:
```c
#include "llmghost-backend-factory.h"
```

- [ ] **Step 6: Build and run the full unit suite**

Run:
```bash
ninja -C build
meson test -C build --suite unit -v
```
Expected: PASS — every unit test, including the 5 new factory tests.

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-backend-factory.h lib/llmghost-backend-factory.c lib/llmghost.h lib/meson.build tests/test-settings.c
git commit -m "feat(settings): backend factory building from settings"
```

---

## Task 5: Plugin integration

Replace the hardcoded backend with one built from settings, and rebuild + reattach controllers on live reload.

**Files:**
- Modify: `plugin/llmghost-plugin.c`

> **Manual-verify note:** the live-reload-rebuilds-completions behavior requires a running gedit and cannot be exercised by the automated suite (no display over SSH). The gate here is: the project compiles and the existing `controller` gui test still passes. Behavioral verification is manual.

- [ ] **Step 1: Add settings state to the plugin struct**

In `plugin/llmghost-plugin.c`, extend `struct _LlmghostPlugin` (currently around lines 22-31) to:

```c
struct _LlmghostPlugin
{
  PeasExtensionBase  parent_instance;

  GeditWindow       *window;          /* injected by libpeas via PROP_WINDOW */
  LlmGhostSettings  *settings;        /* owns the config file + monitor */
  LlmGhostBackend   *backend;         /* rebuilt from settings on "changed" */

  gulong             h_tab_added;
  gulong             h_tab_removed;
  gulong             h_settings_changed;
};
```

- [ ] **Step 2: Apply debounce when attaching, and add a reattach-all helper**

Replace the `attach_controller` function (currently lines 45-59) with a version that applies the configured debounce, and add `reattach_all` after `detach_controller`:

```c
static void
attach_controller (LlmghostPlugin *self, GeditView *view)
{
  if (g_object_get_data (G_OBJECT (view), CONTROLLER_DATA_KEY) != NULL)
    return;

  LlmGhostController *ctrl = llm_ghost_controller_new (
    GTK_TEXT_VIEW (view), self->backend);

  guint ms;
  if (llm_ghost_settings_get_debounce_ms (self->settings, &ms))
    llm_ghost_controller_set_debounce_ms (ctrl, ms);

  /* Lifetime tied to the view: when the view is destroyed (tab close,
   * window close), the destroy notify drops the last ref on the
   * controller, which disconnects its signal handlers in finalize. */
  g_object_set_data_full (G_OBJECT (view), CONTROLLER_DATA_KEY,
                          ctrl, g_object_unref);
}

/* Tear down and rebuild every view's controller so each binds to the current
 * self->backend. Called after a settings reload swaps the backend. */
static void
reattach_all (LlmghostPlugin *self)
{
  GList *views = gedit_window_get_views (self->window);
  for (GList *l = views; l != NULL; l = l->next)
    {
      detach_controller (GEDIT_VIEW (l->data));
      attach_controller (self, GEDIT_VIEW (l->data));
    }
  g_list_free (views);
}
```

(Leave `detach_controller` itself unchanged.)

- [ ] **Step 3: Add the settings-changed handler**

Add this function just above `llmghost_plugin_activate`:

```c
static void
on_settings_changed (LlmGhostSettings *settings, gpointer user_data)
{
  (void) settings;
  LlmghostPlugin *self = LLMGHOST_PLUGIN (user_data);

  /* Build the new backend, then swap every controller onto it. */
  g_clear_object (&self->backend);
  self->backend = llm_ghost_backend_new_from_settings (self->settings);
  reattach_all (self);
}
```

- [ ] **Step 4: Build settings + factory in activate**

Replace the body of `llmghost_plugin_activate` (currently lines 90-109) with:

```c
static void
llmghost_plugin_activate (GeditWindowActivatable *activatable)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (activatable);

  /* One settings object (and one backend) serves every view in this window. */
  self->settings = llm_ghost_settings_new (NULL);
  self->backend  = llm_ghost_backend_new_from_settings (self->settings);

  GList *views = gedit_window_get_views (self->window);
  for (GList *l = views; l != NULL; l = l->next)
    attach_controller (self, GEDIT_VIEW (l->data));
  g_list_free (views);

  self->h_tab_added = g_signal_connect (self->window, "tab-added",
                                        G_CALLBACK (on_tab_added), self);
  self->h_tab_removed = g_signal_connect (self->window, "tab-removed",
                                          G_CALLBACK (on_tab_removed), self);
  self->h_settings_changed = g_signal_connect (self->settings, "changed",
                                               G_CALLBACK (on_settings_changed), self);
}
```

- [ ] **Step 5: Tear down settings in deactivate**

Replace the body of `llmghost_plugin_deactivate` (currently lines 111-133) with:

```c
static void
llmghost_plugin_deactivate (GeditWindowActivatable *activatable)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (activatable);

  if (self->h_settings_changed != 0)
    {
      g_signal_handler_disconnect (self->settings, self->h_settings_changed);
      self->h_settings_changed = 0;
    }
  if (self->h_tab_added != 0)
    {
      g_signal_handler_disconnect (self->window, self->h_tab_added);
      self->h_tab_added = 0;
    }
  if (self->h_tab_removed != 0)
    {
      g_signal_handler_disconnect (self->window, self->h_tab_removed);
      self->h_tab_removed = 0;
    }

  GList *views = gedit_window_get_views (self->window);
  for (GList *l = views; l != NULL; l = l->next)
    detach_controller (GEDIT_VIEW (l->data));
  g_list_free (views);

  g_clear_object (&self->backend);
  g_clear_object (&self->settings);   /* drops the GFileMonitor */
}
```

- [ ] **Step 6: Drop settings in finalize too**

Replace `llmghost_plugin_finalize` (currently lines 178-184) with:

```c
static void
llmghost_plugin_finalize (GObject *object)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (object);
  g_clear_object (&self->backend);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (llmghost_plugin_parent_class)->finalize (object);
}
```

- [ ] **Step 7: Build and run the full suite**

Run:
```bash
ninja -C build
meson test -C build -v
```
Expected: PASS — all unit tests plus the `gui` `controller` test (Xvfb-wrapped) still green. The plugin shared module compiles and links.

- [ ] **Step 8: Commit**

```bash
git add plugin/llmghost-plugin.c
git commit -m "feat(plugin): build backend from settings, rebuild on live reload"
```

---

## Task 6: Preferences "Open settings.json" button

A `PeasGtkConfigurable` extension so gedit's plugin manager shows a Preferences button that opens the settings file.

**Files:**
- Create: `plugin/llmghost-configurable.h`
- Create: `plugin/llmghost-configurable.c`
- Modify: `plugin/llmghost-plugin.c` (register the extension)
- Modify: `meson.build` (add `peas_gtk_dep`)
- Modify: `plugin/meson.build` (add source + dep)

> **Manual-verify note:** the widget and file-open behavior require a running gedit; the automated gate is compile + link only. Behavioral verification is manual.

- [ ] **Step 1: Create `plugin/llmghost-configurable.h`**

```c
#pragma once

#include <glib-object.h>
#include <gmodule.h>
#include <libpeas/peas.h>

G_BEGIN_DECLS

#define LLMGHOST_TYPE_CONFIGURABLE (llmghost_configurable_get_type())
G_DECLARE_FINAL_TYPE (LlmghostConfigurable, llmghost_configurable,
                      LLMGHOST, CONFIGURABLE, PeasExtensionBase)

/* Register the type and the PeasGtkConfigurable extension on @module. */
void llmghost_configurable_register (PeasObjectModule *module);

G_END_DECLS
```

- [ ] **Step 2: Create `plugin/llmghost-configurable.c`**

```c
/* Preferences entry point: a PeasGtkConfigurable that shows the settings-file
 * path and an "Open settings.json" button. The file IS the UI in v1 — there is
 * no field-by-field widget. */

#include "llmghost-configurable.h"

#include <gtk/gtk.h>
#include <libpeas-gtk/peas-gtk.h>

#include "llmghost.h"   /* llm_ghost_settings_new / _default_path */

struct _LlmghostConfigurable
{
  PeasExtensionBase parent_instance;
};

static void configurable_iface_init (PeasGtkConfigurableInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (
  LlmghostConfigurable,
  llmghost_configurable,
  PEAS_TYPE_EXTENSION_BASE,
  0,
  G_IMPLEMENT_INTERFACE_DYNAMIC (PEAS_GTK_TYPE_CONFIGURABLE,
                                 configurable_iface_init))

static void
on_open_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  const char *path = user_data;

  /* Make sure the file exists (writes the default on first use) before we
   * hand it to the desktop. */
  LlmGhostSettings *s = llm_ghost_settings_new (path);
  g_object_unref (s);

  char *uri = g_filename_to_uri (path, NULL, NULL);
  if (uri != NULL)
    {
      GError *error = NULL;
      if (!gtk_show_uri_on_window (NULL, uri, GDK_CURRENT_TIME, &error))
        {
          g_warning ("could not open %s: %s", uri, error->message);
          g_clear_error (&error);
        }
      g_free (uri);
    }
}

static GtkWidget *
configurable_create_widget (PeasGtkConfigurable *configurable)
{
  (void) configurable;
  char *path = llm_ghost_settings_default_path ();   /* owned by the button below */

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);

  char *markup = g_strdup_printf ("Settings file:\n<tt>%s</tt>", path);
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  g_free (markup);

  GtkWidget *button = gtk_button_new_with_label ("Open settings.json");
  /* The closure owns `path` and frees it when the button is destroyed. */
  g_signal_connect_data (button, "clicked", G_CALLBACK (on_open_clicked),
                         path, (GClosureNotify) g_free, 0);

  gtk_box_pack_start (GTK_BOX (box), label,  FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
  gtk_widget_show_all (box);
  return box;
}

static void
configurable_iface_init (PeasGtkConfigurableInterface *iface)
{
  iface->create_configure_widget = configurable_create_widget;
}

static void
llmghost_configurable_init (LlmghostConfigurable *self)
{
  (void) self;
}

static void
llmghost_configurable_class_init (LlmghostConfigurableClass *klass)
{
  (void) klass;
}

static void
llmghost_configurable_class_finalize (LlmghostConfigurableClass *klass)
{
  (void) klass;
}

void
llmghost_configurable_register (PeasObjectModule *module)
{
  llmghost_configurable_register_type (G_TYPE_MODULE (module));
  peas_object_module_register_extension_type (module,
                                              PEAS_GTK_TYPE_CONFIGURABLE,
                                              LLMGHOST_TYPE_CONFIGURABLE);
}
```

- [ ] **Step 3: Register the configurable in `peas_register_types`**

In `plugin/llmghost-plugin.c`, add the include near the other includes:
```c
#include "llmghost-configurable.h"
```

Then in `peas_register_types` (currently lines 213-220), add the registration call so it reads:

```c
G_MODULE_EXPORT void
peas_register_types (PeasObjectModule *module)
{
  llmghost_plugin_register_type (G_TYPE_MODULE (module));
  peas_object_module_register_extension_type (module,
                                              GEDIT_TYPE_WINDOW_ACTIVATABLE,
                                              LLMGHOST_TYPE_PLUGIN);
  llmghost_configurable_register (module);
}
```

- [ ] **Step 4: Add the libpeas-gtk dependency and the new source**

In the root `meson.build`, add after the `peas_dep` line (line 21):
```meson
peas_gtk_dep = dependency('libpeas-gtk-1.0', version: '>= 1.20')
```

In `plugin/meson.build`, add `'llmghost-configurable.c',` to the `shared_module(...)` source list and add `peas_gtk_dep,` to its `dependencies:` list.

- [ ] **Step 5: Build — verify the plugin module links with the configurable**

Run:
```bash
meson setup --reconfigure build
ninja -C build
```
Expected: the `libllmghost.so` plugin module builds and links against libpeas-gtk. No test changes (GUI glue is manually verified).

- [ ] **Step 6: Commit**

```bash
git add plugin/llmghost-configurable.h plugin/llmghost-configurable.c plugin/llmghost-plugin.c meson.build plugin/meson.build
git commit -m "feat(plugin): add PeasGtkConfigurable Open settings.json button"
```

---

## Task 7: Documentation

Record that the settings layer landed and document the schema for users.

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Update NOTES.md**

Open `NOTES.md`, find the Phase 6 prerequisite paragraph describing the deferred JSON settings layer (the text that says the settings layer is a future human-editable JSON config file, chosen over GSettings). Replace that paragraph with a "landed" subsection documenting the schema. Insert this block in its place:

```markdown
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
```

If the surrounding NOTES.md prose references the settings layer as "deferred"
or "still to do" elsewhere (e.g. a checklist line), update those mentions to
reflect that it has landed.

- [ ] **Step 2: Commit**

```bash
git add NOTES.md
git commit -m "docs: document the JSON settings layer and schema"
```

---

## Final Review

After all tasks, dispatch a holistic code reviewer over the whole branch diff, then use **superpowers:finishing-a-development-branch**.

Verify before finishing:
```bash
ninja -C build
meson test -C build -v
```
Expected: the entire suite (unit + gui) passes.

---

## Self-Review (plan vs. spec)

- **Goal / schema / interpolation / reload / factory / Prefs button** — Tasks 1-7 cover each spec section: interpolation (T1), parse+accessors (T2), auto-write+live-reload+last-good (T3), factory GTypes (T4), plugin rebuild-on-change + debounce (T5), `PeasGtkConfigurable` (T6), docs (T7).
- **Test seams** — `_llm_ghost_settings_interpolate`, `_new_from_string`, `_reload` are all in `-internal.h` (not installed), mirroring the existing `*-internal.h` pattern; the factory tests assert GType via `LLM_GHOST_IS_*_BACKEND`, exactly as the spec's testing strategy requires.
- **Manual-verify honesty** — Tasks 5 and 6 explicitly flag that live gedit behavior is compile-checked + manual (no display over SSH), matching the spec's stated limitation.
- **Type consistency** — `LlmGhostSettings`, `llm_ghost_settings_new`, `_get_active_backend`, `_get_debounce_ms`, `_get_backend_params`, `_default_path`, `llm_ghost_backend_new_from_settings`, `llmghost_configurable_register` are spelled identically everywhere they appear across tasks. The `changed` signal is registered in T2 and emitted in T3. `set_root`/`parse_and_interpolate`/`default_object` defined in T2 are reused (not redefined) in T3.
- **No placeholders** — every code step contains complete code; every run step states the exact command and expected outcome.
```
