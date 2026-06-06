# Secret Storage (libsecret) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users keep LLM API keys in the system keyring (gnome-keyring via libsecret), referenced from `settings.json` as `${secret:NAME}` and managed from the Preferences dialog, instead of plaintext config / env vars.

**Architecture:** A thin libsecret wrapper module (`LlmGhostSecretStore`) provides lookup/store/clear. The settings interpolator gains a `${secret:NAME}` branch that resolves through a swappable function pointer (real libsecret by default, a fake in tests). A pure `collect_secret_refs` scan drives a new secrets section in the prefs dialog, whose Store/Clear buttons call the wrapper and then "touch" `settings.json` to trigger the plugin's existing live-reload.

**Tech Stack:** C (gnu11), GLib/GObject, GIO, GTK3, libsecret-1, json-glib, libpeas-gtk, meson/ninja, GLib `g_test`.

**Spec:** `docs/superpowers/specs/2026-06-06-secret-storage-design.md`

---

## Prerequisite (must be done before Task 1)

`libsecret-1` is a hard build dependency and is **not currently installed** on the dev machine. Install the dev package first, e.g. on Debian/Ubuntu:

```
sudo apt install libsecret-1-dev
```

Verify: `pkg-config --exists libsecret-1 && echo OK`. Until this passes, nothing in this plan compiles.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-secret-store.{c,h}` (new) | libsecret wrapper: schema + `llm_ghost_secret_lookup/store/clear`. Header internal (not installed). |
| `tests/test-secret-store.c` (new) | Round-trip test, skipped when no secret service is available. |
| `lib/llmghost-settings.c` (modify) | `${secret:NAME}` interpolation branch + swappable secret-source; `collect_secret_refs`; `llm_ghost_settings_touch`. |
| `lib/llmghost-settings-internal.h` (modify) | test-only seam: secret-lookup setter + `collect_secret_refs` decl. |
| `lib/llmghost-settings.h` (modify) | public `llm_ghost_settings_touch`. |
| `tests/test-settings.c` (modify) | `${secret:}` interpolation, `collect_secret_refs`, and `touch` tests. |
| `plugin/llmghost-configurable.c` (modify) | Secrets section in the prefs dialog; post-store file touch. |
| `meson.build`, `lib/meson.build`, `tests/meson.build` (modify) | `libsecret-1` dep + new source/test wiring. |
| `NOTES.md` (modify) | Document the feature; mark prerequisite #1 landed. |

**Conventions (verified in-tree):** internal headers (`*-internal.h`, `llmghost-http-util.h`, `llmghost-text-util.h`) are NOT added to `llmghost_headers`; `#define G_LOG_DOMAIN` at the top of each `.c`; the warning-determinism test convention (`g_test_expect_message` + matching our own lowercase message text). The settings file monitor (`lib/llmghost-settings.c:299-304`) reacts only to `G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT` and `_CREATED` — an mtime-only touch (`ATTRIBUTE_CHANGED`) would be ignored, so the reload trigger must rewrite the file's bytes.

---

## Task 1: `LlmGhostSecretStore` — libsecret wrapper

The isolated libsecret edge. Everything else talks to it through three functions.

**Files:**
- Create: `lib/llmghost-secret-store.h`, `lib/llmghost-secret-store.c`, `tests/test-secret-store.c`
- Modify: `meson.build`, `lib/meson.build`, `tests/meson.build`

- [ ] **Step 1: Add the libsecret dependency to meson**

In `meson.build`, after the `json_dep` line (line 19), add:

```meson
secret_dep  = dependency('libsecret-1', version: '>= 0.20')
```

In `lib/meson.build`, add `secret_dep` to the `llmghost_deps` list:

```meson
llmghost_deps = [
  gtk_dep, glib_dep, gobject_dep, gio_dep, soup_dep, json_dep, secret_dep,
]
```

(The plugin and tests link `llmghost_dep`, a `declare_dependency` that re-exports `llmghost_deps`, so libsecret propagates automatically — no change needed in `plugin/meson.build`.)

- [ ] **Step 2: Create the header `lib/llmghost-secret-store.h`**

```c
#pragma once

/* libsecret-backed secret storage, keyed by a short NAME. Internal (NOT added
 * to installed llmghost_headers); used by the settings interpolator, the prefs
 * dialog, and tests. All calls are synchronous. */

#include <glib.h>

G_BEGIN_DECLS

/* Look up the secret stored under @name. Returns a newly-allocated value
 * (free with g_free), or NULL if not found or on error (then *error is set
 * only on a real error, not on "not found"). */
char     *llm_ghost_secret_lookup (const char *name, GError **error);

/* Store/overwrite @value under @name in the default collection. */
gboolean  llm_ghost_secret_store  (const char *name, const char *value, GError **error);

/* Remove the secret under @name. Removing an absent key is success (idempotent). */
gboolean  llm_ghost_secret_clear  (const char *name, GError **error);

G_END_DECLS
```

- [ ] **Step 3: Write the failing round-trip test `tests/test-secret-store.c`**

```c
#include <glib.h>
#include "llmghost-secret-store.h"

#define TEST_NAME "llmghost-selftest-key"

/* Is a secret service reachable? Headless CI / no D-Bus session / no
 * gnome-keyring → skip. A reachable service returns NULL + no error for a
 * missing key; an unreachable one sets an error. */
static gboolean
service_available (void)
{
  GError *error = NULL;
  char *v = llm_ghost_secret_lookup ("llmghost-probe-nonexistent", &error);
  g_free (v);
  if (error != NULL)
    {
      g_clear_error (&error);
      return FALSE;
    }
  return TRUE;
}

static void
test_round_trip (void)
{
  if (!service_available ())
    {
      g_test_skip ("no secret service available");
      return;
    }
  GError *error = NULL;

  g_assert_true (llm_ghost_secret_store (TEST_NAME, "sk-secret-123", &error));
  g_assert_no_error (error);

  char *v = llm_ghost_secret_lookup (TEST_NAME, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (v, ==, "sk-secret-123");
  g_free (v);

  g_assert_true (llm_ghost_secret_clear (TEST_NAME, &error));
  g_assert_no_error (error);

  v = llm_ghost_secret_lookup (TEST_NAME, &error);
  g_assert_no_error (error);
  g_assert_null (v);                       /* gone */
  g_free (v);

  g_assert_true (llm_ghost_secret_clear (TEST_NAME, &error));   /* idempotent */
  g_assert_no_error (error);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/secret-store/round-trip", test_round_trip);
  return g_test_run ();
}
```

Wire it into `tests/meson.build` (append after the `test_generic_body` block):

```meson
test_secret_store = executable(
  'test-secret-store',
  'test-secret-store.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('secret-store', test_secret_store, suite: 'unit')
```

- [ ] **Step 4: Run to verify it fails to link**

Run: `ninja -C build`
Expected: FAIL — undefined reference to `llm_ghost_secret_lookup` / `_store` / `_clear` (the `.c` doesn't exist yet).

- [ ] **Step 5: Implement `lib/llmghost-secret-store.c`**

```c
#define G_LOG_DOMAIN "llmghost-secret"

/* Acknowledge libsecret's "API subject to change" guard before the include. */
#define SECRET_API_SUBJECT_TO_CHANGE
#include "llmghost-secret-store.h"

#include <libsecret/secret.h>

/* One schema for all llmghost secrets, keyed by a single "name" attribute. */
static const SecretSchema *
llm_ghost_secret_schema (void)
{
  static const SecretSchema schema = {
    "de.mickautsch.llmghost", SECRET_SCHEMA_NONE,
    {
      { "name", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 },
    }
  };
  return &schema;
}

char *
llm_ghost_secret_lookup (const char *name, GError **error)
{
  g_return_val_if_fail (name != NULL, NULL);
  char *secret = secret_password_lookup_sync (llm_ghost_secret_schema (),
                                              NULL, error,
                                              "name", name, NULL);
  if (secret == NULL)
    return NULL;                          /* not found, or *error set */
  char *out = g_strdup (secret);          /* normalize ownership to g_free */
  secret_password_free (secret);
  return out;
}

gboolean
llm_ghost_secret_store (const char *name, const char *value, GError **error)
{
  g_return_val_if_fail (name != NULL && value != NULL, FALSE);
  char *label = g_strdup_printf ("llmghost: %s", name);
  gboolean ok = secret_password_store_sync (llm_ghost_secret_schema (),
                                           SECRET_COLLECTION_DEFAULT, label, value,
                                           NULL, error,
                                           "name", name, NULL);
  g_free (label);
  return ok;
}

gboolean
llm_ghost_secret_clear (const char *name, GError **error)
{
  g_return_val_if_fail (name != NULL, FALSE);
  /* clear_sync returns FALSE with no error when nothing matched — treat that
   * "nothing to clear" as success; only a set GError is a real failure. */
  GError *local = NULL;
  secret_password_clear_sync (llm_ghost_secret_schema (), NULL, &local,
                              "name", name, NULL);
  if (local != NULL)
    {
      g_propagate_error (error, local);
      return FALSE;
    }
  return TRUE;
}
```

Add the source to `lib/meson.build`'s `llmghost_sources` (do NOT add the header to `llmghost_headers` — it is internal):

```meson
  'llmghost-secret-store.c',
```

- [ ] **Step 6: Build and run**

Run: `ninja -C build && meson test -C build --suite unit secret-store -v`
Expected: PASS — `/secret-store/round-trip` either runs green (a secret service is up) or reports `SKIP: no secret service available`. Either is a pass. Then `meson test -C build` → all suites still pass.

- [ ] **Step 7: Commit**

```bash
git add meson.build lib/meson.build tests/meson.build \
        lib/llmghost-secret-store.h lib/llmghost-secret-store.c tests/test-secret-store.c
git commit -m "feat(secret): libsecret-backed secret store (lookup/store/clear)"
```

---

## Task 2: `${secret:NAME}` interpolation + swappable secret source

Resolve `${secret:NAME}` to a keyring value at settings load-time, reusing the existing interpolation pass. A function-pointer seam keeps it testable without a daemon.

**Files:**
- Modify: `lib/llmghost-settings-internal.h`, `lib/llmghost-settings.c`, `tests/test-settings.c`

- [ ] **Step 1: Declare the test seam in `lib/llmghost-settings-internal.h`**

Add before `G_END_DECLS`:

```c
/* Secret-source seam (testing). The interpolator resolves ${secret:NAME} via
 * this function; the default is a libsecret-backed lookup. The fn returns a
 * newly-allocated value (g_free) or NULL when the secret is unavailable (the
 * interpolator then warns and substitutes ""). Pass NULL to restore the
 * default. */
typedef char *(*LlmGhostSecretLookupFn) (const char *name);
void _llm_ghost_settings_set_secret_lookup_for_testing (LlmGhostSecretLookupFn fn);
```

- [ ] **Step 2: Add failing interpolation tests to `tests/test-settings.c`**

Add a fake lookup and four tests (place them after `test_interpolate_literal_dollar`):

```c
static char *
fake_secret_lookup (const char *name)
{
  if (g_strcmp0 (name, "openai") == 0)
    return g_strdup ("sk-from-keyring");
  return NULL;                              /* unknown → unavailable */
}

static void
test_interpolate_secret_found (void)
{
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  char *r = _llm_ghost_settings_interpolate ("Bearer ${secret:openai}!");
  g_assert_cmpstr (r, ==, "Bearer sk-from-keyring!");
  g_free (r);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
}

static void
test_interpolate_secret_missing (void)
{
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not available*");
  char *r = _llm_ghost_settings_interpolate ("k=${secret:nope};");
  g_test_assert_expected_messages ();
  g_assert_cmpstr (r, ==, "k=;");
  g_free (r);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
}

static void
test_interpolate_secret_and_env_coexist (void)
{
  g_setenv ("LLMGHOST_TEST_E", "ENV", TRUE);
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  char *r = _llm_ghost_settings_interpolate ("${secret:openai}/${LLMGHOST_TEST_E}");
  g_assert_cmpstr (r, ==, "sk-from-keyring/ENV");
  g_free (r);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
  g_unsetenv ("LLMGHOST_TEST_E");
}

static void
test_interpolate_secret_in_array (void)
{
  /* ${secret:} nested inside a backends params array+object (generic shape). */
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backends\":{\"generic\":{\"request_template\":"
      "{\"messages\":[{\"content\":\"${secret:openai}\"}]}}}}");
  JsonObject *p = llm_ghost_settings_get_backend_params (s, "generic");
  JsonObject *t = json_object_get_object_member (p, "request_template");
  JsonArray  *m = json_object_get_array_member (t, "messages");
  JsonObject *m0 = json_array_get_object_element (m, 0);
  g_assert_cmpstr (json_object_get_string_member (m0, "content"), ==, "sk-from-keyring");
  g_object_unref (s);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
}
```

Register them in `main` (after the `interpolate/literal-dollar` line):

```c
  g_test_add_func ("/settings/interpolate/secret-found",   test_interpolate_secret_found);
  g_test_add_func ("/settings/interpolate/secret-missing", test_interpolate_secret_missing);
  g_test_add_func ("/settings/interpolate/secret-env",     test_interpolate_secret_and_env_coexist);
  g_test_add_func ("/settings/interpolate/secret-array",   test_interpolate_secret_in_array);
```

- [ ] **Step 3: Run to verify it fails**

Run: `ninja -C build`
Expected: FAIL — undefined reference to `_llm_ghost_settings_set_secret_lookup_for_testing` (and `${secret:openai}` would currently be treated as an env var, so the assertions would fail too once it links).

- [ ] **Step 4: Implement the secret source + `${secret:}` branch in `lib/llmghost-settings.c`**

Add the include near the top (with the other includes):

```c
#include "llmghost-secret-store.h"
```

Add the seam directly above `_llm_ghost_settings_interpolate` (i.e. just under the `/* ---- ${ENV} interpolation ---- */` banner):

```c
static char *
default_secret_lookup (const char *name)
{
  return llm_ghost_secret_lookup (name, NULL);   /* NULL on any failure */
}

static LlmGhostSecretLookupFn secret_lookup_fn = default_secret_lookup;

void
_llm_ghost_settings_set_secret_lookup_for_testing (LlmGhostSecretLookupFn fn)
{
  secret_lookup_fn = fn != NULL ? fn : default_secret_lookup;
}
```

Replace the body of the `if (end != NULL)` block inside `_llm_ghost_settings_interpolate` (the part that currently does `g_getenv` and appends) with a prefix-aware version. The full updated loop body:

```c
      if (p[0] == '$' && p[1] == '{')
        {
          const char *end = strchr (p + 2, '}');
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              if (g_str_has_prefix (name, "secret:"))
                {
                  const char *sname = name + strlen ("secret:");
                  char *val = secret_lookup_fn (sname);
                  if (val == NULL)
                    g_warning ("secret ${secret:%s} is not available; using \"\"", sname);
                  g_string_append (out, val != NULL ? val : "");
                  g_free (val);
                }
              else
                {
                  const char *envv = g_getenv (name);
                  if (envv == NULL)
                    {
                      g_warning ("environment variable ${%s} is not set; using \"\"", name);
                      envv = "";
                    }
                  g_string_append (out, envv);
                }
              g_free (name);
              p = end + 1;
              continue;
            }
        }
      g_string_append_c (out, *p);
      p++;
```

(`${ENV}` behavior is unchanged — same message, same `""` fallback. `g_free (val)` on a NULL pointer is a documented no-op.)

- [ ] **Step 5: Build and run**

Run: `ninja -C build && meson test -C build --suite unit settings -v`
Expected: PASS — the four new `/settings/interpolate/secret-*` cases plus all existing settings cases. Then `meson test -C build` → all suites pass.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-settings-internal.h lib/llmghost-settings.c tests/test-settings.c
git commit -m "feat(settings): resolve \${secret:NAME} via a swappable secret source"
```

---

## Task 3: `collect_secret_refs` — pure scan for the dialog

A pure function that returns the distinct `${secret:NAME}` names in a (raw, un-interpolated) settings tree, so the dialog can offer exactly those keys.

**Files:**
- Modify: `lib/llmghost-settings-internal.h`, `lib/llmghost-settings.c`, `tests/test-settings.c`

- [ ] **Step 1: Declare it in `lib/llmghost-settings-internal.h`**

Add before `G_END_DECLS` (it needs json-glib — the header already includes `llmghost-settings.h`, which includes `<json-glib/json-glib.h>`):

```c
/* Collect distinct ${secret:NAME} names referenced in any string value of
 * @root (recursing objects + arrays). Returns a newly-allocated,
 * NULL-terminated, de-duplicated array (g_strfreev). Never NULL (may be empty).
 * @root may be NULL (→ empty). Scan the RAW parse, before interpolation. */
char **_llm_ghost_settings_collect_secret_refs (JsonObject *root);
```

- [ ] **Step 2: Add failing tests to `tests/test-settings.c`**

Add a raw-parse helper, a strv-contains helper, and three tests (place before `main`):

```c
static JsonObject *
raw_object (const char *json)
{
  JsonParser *parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, NULL));
  JsonObject *o = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  return o;
}

static gboolean
strv_contains (char **v, const char *s)
{
  for (int i = 0; v[i] != NULL; i++)
    if (g_strcmp0 (v[i], s) == 0)
      return TRUE;
  return FALSE;
}

static void
test_collect_refs_basic (void)
{
  JsonObject *o = raw_object (
    "{\"a\":\"${secret:one}\",\"b\":{\"c\":\"x ${secret:two} y\"},"
     "\"d\":[\"${secret:one}\",\"${ENV_X}\"]}");
  char **refs = _llm_ghost_settings_collect_secret_refs (o);
  g_assert_cmpint (g_strv_length (refs), ==, 2);     /* one, two — deduped */
  g_assert_true (strv_contains (refs, "one"));
  g_assert_true (strv_contains (refs, "two"));
  g_assert_false (strv_contains (refs, "ENV_X"));     /* env vars are not secrets */
  g_strfreev (refs);
  json_object_unref (o);
}

static void
test_collect_refs_none (void)
{
  JsonObject *o = raw_object ("{\"a\":\"plain\",\"b\":\"${ENV_ONLY}\"}");
  char **refs = _llm_ghost_settings_collect_secret_refs (o);
  g_assert_cmpint (g_strv_length (refs), ==, 0);
  g_assert_null (refs[0]);
  g_strfreev (refs);
  json_object_unref (o);
}

static void
test_collect_refs_null (void)
{
  char **refs = _llm_ghost_settings_collect_secret_refs (NULL);
  g_assert_nonnull (refs);
  g_assert_null (refs[0]);
  g_strfreev (refs);
}
```

Register in `main`:

```c
  g_test_add_func ("/settings/secret-refs/basic", test_collect_refs_basic);
  g_test_add_func ("/settings/secret-refs/none",  test_collect_refs_none);
  g_test_add_func ("/settings/secret-refs/null",  test_collect_refs_null);
```

- [ ] **Step 3: Run to verify it fails**

Run: `ninja -C build`
Expected: FAIL — undefined reference to `_llm_ghost_settings_collect_secret_refs`.

- [ ] **Step 4: Implement in `lib/llmghost-settings.c`**

Add after `interpolate_object` (it needs `<string.h>` for `strstr`/`strchr`, already included by the file). Place under a banner:

```c
/* ---- ${secret:NAME} reference collection (for the prefs dialog) --------- */

static void
collect_refs_from_string (const char *s, GHashTable *set)
{
  const char *p = s;
  while ((p = strstr (p, "${secret:")) != NULL)
    {
      const char *start = p + strlen ("${secret:");
      const char *end = strchr (start, '}');
      if (end == NULL)
        break;
      char *name = g_strndup (start, (gsize) (end - start));
      if (*name != '\0' && !g_hash_table_contains (set, name))
        g_hash_table_add (set, name);     /* set takes ownership */
      else
        g_free (name);
      p = end + 1;
    }
}

static void
collect_refs_from_node (JsonNode *node, GHashTable *set)
{
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      JsonObject *obj = json_node_get_object (node);
      GList *members = json_object_get_members (obj);
      for (GList *l = members; l != NULL; l = l->next)
        collect_refs_from_node (json_object_get_member (obj, l->data), set);
      g_list_free (members);
    }
  else if (JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *arr = json_node_get_array (node);
      guint n = json_array_get_length (arr);
      for (guint i = 0; i < n; i++)
        collect_refs_from_node (json_array_get_element (arr, i), set);
    }
  else if (JSON_NODE_HOLDS_VALUE (node) &&
           json_node_get_value_type (node) == G_TYPE_STRING)
    {
      collect_refs_from_string (json_node_get_string (node), set);
    }
}

char **
_llm_ghost_settings_collect_secret_refs (JsonObject *root)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (root != NULL)
    {
      GList *members = json_object_get_members (root);
      for (GList *l = members; l != NULL; l = l->next)
        collect_refs_from_node (json_object_get_member (root, l->data), set);
      g_list_free (members);
    }

  char **out = g_new0 (char *, g_hash_table_size (set) + 1);
  GHashTableIter it;
  gpointer key;
  guint i = 0;
  g_hash_table_iter_init (&it, set);
  while (g_hash_table_iter_next (&it, &key, NULL))
    out[i++] = g_strdup ((char *) key);
  out[i] = NULL;

  g_hash_table_destroy (set);             /* frees the owned key strings */
  return out;
}
```

- [ ] **Step 5: Build and run**

Run: `ninja -C build && meson test -C build --suite unit settings -v`
Expected: PASS — the three new `/settings/secret-refs/*` cases plus all prior settings cases. Then `meson test -C build` → all suites pass.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-settings-internal.h lib/llmghost-settings.c tests/test-settings.c
git commit -m "feat(settings): collect_secret_refs scan for the prefs dialog"
```

---

## Task 4: `llm_ghost_settings_touch` — reload trigger

A public helper the dialog calls after a store/clear so the running plugin re-interpolates (the keyring changed but the file did not). It rewrites the file's current bytes, which fires the monitor's `CHANGES_DONE_HINT`.

**Files:**
- Modify: `lib/llmghost-settings.h`, `lib/llmghost-settings.c`, `tests/test-settings.c`

- [ ] **Step 1: Declare it in `lib/llmghost-settings.h`**

Add before `G_END_DECLS`:

```c
/* Rewrite the file at @path with its current contents, to trigger any
 * GFileMonitor watching it (a CHANGES_DONE_HINT) so a keyring change that did
 * not modify the file still causes a live reload + re-interpolation. Content is
 * preserved. Returns FALSE + @error if the file cannot be read or written. */
gboolean llm_ghost_settings_touch (const char *path, GError **error);
```

- [ ] **Step 2: Add failing tests to `tests/test-settings.c`**

`write_temp_settings` already exists in this file. Add (before `main`):

```c
static void
test_touch_preserves_content (void)
{
  char *path = write_temp_settings ("{\"backend\":\"openai\"}");
  GError *error = NULL;
  g_assert_true (llm_ghost_settings_touch (path, &error));
  g_assert_no_error (error);

  char *data = NULL;
  g_assert_true (g_file_get_contents (path, &data, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (data, ==, "{\"backend\":\"openai\"}");

  g_free (data);
  g_free (path);
}

static void
test_touch_missing_file_errors (void)
{
  GError *error = NULL;
  g_assert_false (llm_ghost_settings_touch ("/nonexistent/llmghost/x.json", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
}
```

Register in `main`:

```c
  g_test_add_func ("/settings/touch/preserves", test_touch_preserves_content);
  g_test_add_func ("/settings/touch/missing",   test_touch_missing_file_errors);
```

- [ ] **Step 3: Run to verify it fails**

Run: `ninja -C build`
Expected: FAIL — undefined reference to `llm_ghost_settings_touch`.

- [ ] **Step 4: Implement in `lib/llmghost-settings.c`**

Add near the other file helpers (after `parse_and_interpolate`, anywhere at file scope):

```c
gboolean
llm_ghost_settings_touch (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, error))
    return FALSE;
  gboolean ok = g_file_set_contents (path, data, (gssize) len, error);
  g_free (data);
  return ok;
}
```

- [ ] **Step 5: Build and run**

Run: `ninja -C build && meson test -C build --suite unit settings -v`
Expected: PASS — the two new `/settings/touch/*` cases plus all prior settings cases. Then `meson test -C build` → all suites pass.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-settings.h lib/llmghost-settings.c tests/test-settings.c
git commit -m "feat(settings): llm_ghost_settings_touch to trigger live reload"
```

---

## Task 5: Preferences dialog — Secrets section

Wire the prefs dialog: list the config's `${secret:NAME}` refs, each with a masked entry + Store + Clear, and re-interpolate after a change. GTK glue only — every decision delegates to the unit-tested functions from Tasks 1–4; there is no new unit test (the widget surface is untestable over SSH, consistent with the existing button).

**Files:**
- Modify: `plugin/llmghost-configurable.c`

- [ ] **Step 1: Add includes**

Near the existing includes in `plugin/llmghost-configurable.c`:

```c
#include "llmghost-settings-internal.h"   /* _llm_ghost_settings_collect_secret_refs */
#include "llmghost-secret-store.h"          /* llm_ghost_secret_lookup/store/clear */
```

(`llm_ghost_settings_touch` and `llm_ghost_settings_default_path` come via the already-included `llmghost.h`.)

- [ ] **Step 2: Add the raw-load helper, per-row state, and callbacks**

Insert above `configurable_create_widget`:

```c
/* Parse the settings file WITHOUT interpolation (so ${secret:NAME} survives)
 * into an owned JsonObject, or NULL. */
static JsonObject *
load_raw_settings (const char *path)
{
  JsonParser *parser = json_parser_new ();
  JsonObject *obj = NULL;
  if (json_parser_load_from_file (parser, path, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        obj = json_object_ref (json_node_get_object (root));
    }
  g_object_unref (parser);
  return obj;
}

typedef struct
{
  char     *name;     /* secret name (owned) */
  char     *path;     /* settings path (owned) */
  GtkEntry *entry;    /* not owned (lives in the widget tree) */
  GtkLabel *status;   /* not owned */
} SecretRow;

static void
secret_row_destroy (gpointer data)
{
  SecretRow *r = data;
  g_free (r->name);
  g_free (r->path);
  g_free (r);
}

static void
secret_row_update_status (SecretRow *r)
{
  char *v = llm_ghost_secret_lookup (r->name, NULL);
  gtk_label_set_text (r->status, v != NULL ? "stored" : "not stored");
  g_free (v);
}

static void
on_secret_store_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SecretRow *r = user_data;
  const char *val = gtk_entry_get_text (r->entry);
  if (*val == '\0')
    {
      gtk_label_set_text (r->status, "enter a value first");
      return;
    }

  GError *error = NULL;
  if (!llm_ghost_secret_store (r->name, val, &error))
    {
      gtk_label_set_text (r->status, error != NULL ? error->message : "store failed");
      g_clear_error (&error);
      return;
    }

  gtk_entry_set_text (r->entry, "");                 /* don't keep it on screen */
  llm_ghost_settings_touch (r->path, NULL);          /* apply without restart */
  secret_row_update_status (r);
}

static void
on_secret_clear_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SecretRow *r = user_data;
  GError *error = NULL;
  if (!llm_ghost_secret_clear (r->name, &error))
    {
      gtk_label_set_text (r->status, error != NULL ? error->message : "clear failed");
      g_clear_error (&error);
      return;
    }
  llm_ghost_settings_touch (r->path, NULL);
  secret_row_update_status (r);
}

/* Build the "Secrets" section for the given settings @path. */
static GtkWidget *
build_secrets_section (const char *path)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  GtkWidget *heading = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (heading), "<b>Secrets (keyring)</b>");
  gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
  gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

  JsonObject *raw = load_raw_settings (path);
  char **names = _llm_ghost_settings_collect_secret_refs (raw);
  if (raw != NULL)
    json_object_unref (raw);

  if (names[0] == NULL)
    {
      GtkWidget *hint = gtk_label_new (
        "No ${secret:NAME} references in settings.json. Add one — e.g. "
        "\"api_key\": \"${secret:openai}\" — to manage that key here.");
      gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
      gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
      gtk_box_pack_start (GTK_BOX (box), hint, FALSE, FALSE, 0);
      g_strfreev (names);
      return box;
    }

  for (int i = 0; names[i] != NULL; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

      GtkWidget *label = gtk_label_new (names[i]);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_size_request (label, 120, -1);

      GtkWidget *entry = gtk_entry_new ();
      gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
      gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "enter key…");
      gtk_widget_set_hexpand (entry, TRUE);

      GtkWidget *store_btn = gtk_button_new_with_label ("Store");
      GtkWidget *clear_btn = gtk_button_new_with_label ("Clear");
      GtkWidget *status    = gtk_label_new (NULL);

      SecretRow *r = g_new0 (SecretRow, 1);
      r->name   = g_strdup (names[i]);
      r->path   = g_strdup (path);
      r->entry  = GTK_ENTRY (entry);
      r->status = GTK_LABEL (status);
      /* The row box owns the SecretRow; both buttons borrow it. */
      g_object_set_data_full (G_OBJECT (row), "secret-row", r, secret_row_destroy);

      g_signal_connect (store_btn, "clicked", G_CALLBACK (on_secret_store_clicked), r);
      g_signal_connect (clear_btn, "clicked", G_CALLBACK (on_secret_clear_clicked), r);

      gtk_box_pack_start (GTK_BOX (row), label,     FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (row), entry,     TRUE,  TRUE,  0);
      gtk_box_pack_start (GTK_BOX (row), store_btn, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (row), clear_btn, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (row), status,    FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);

      secret_row_update_status (r);
    }

  g_strfreev (names);
  return box;
}
```

- [ ] **Step 3: Pack the section into the dialog**

In `configurable_create_widget`, after the existing
`gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);` line and before
`gtk_widget_show_all (box);`, add:

```c
  gtk_box_pack_start (GTK_BOX (box),
                      gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), build_secrets_section (path), FALSE, FALSE, 0);
```

(`path` is read synchronously here; `build_secrets_section` copies it into each row, so the button closure's ownership of `path` is unaffected.)

- [ ] **Step 4: Build and run the full suite**

Run: `ninja -C build && meson test -C build`
Expected: the plugin (`libllmghost.so`) compiles and links against libsecret; all 13 test suites pass (the GTK dialog itself has no unit test, by design). Confirm there are no new compiler warnings in `llmghost-configurable.c`.

- [ ] **Step 5: Commit**

```bash
git add plugin/llmghost-configurable.c
git commit -m "feat(prefs): Secrets section to store/clear keyring keys"
```

---

## Task 6: Documentation

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Add a feature section to `NOTES.md`**

After the "Generic (template) backend (landed)" section (it ends with the
paragraph about hand-written Claude/Gemini backends no longer being needed),
insert:

```markdown
### Secret storage (libsecret) (landed)

API keys can live in the system keyring (gnome-keyring) instead of plaintext
config or env vars. Reference a stored key from any string value with
`${secret:NAME}` — resolved at settings load-time in the same interpolation pass
as `${ENV}` (and, like `${ENV}`, a missing/unavailable secret logs a warning and
expands to `""`). Manage keys from the plugin's Preferences page: it lists every
`${secret:NAME}` the active `settings.json` references and offers a masked field
with **Store** / **Clear** per name. Storing or clearing rewrites the settings
file to trigger the live-reload, so a new key takes effect without a restart.
Example: `"api_key": "${secret:openai}"`, then store the value once under the
name `openai` in Preferences (or `secret-tool store --label='llmghost: openai'
name openai`). Backed by `lib/llmghost-secret-store.{c,h}` (a `libsecret-1`
wrapper). Hard build dependency: `libsecret-1`.
```

Then in "Remaining architectural prerequisites", change item #1 from a
prerequisite into a landed note:

```markdown
1. **Secret storage**: API keys via libsecret (`gnome-keyring`). ✓ landed
   2026-06-06 — `${secret:NAME}` config refs + a Preferences manager. Plaintext
   in config / env vars still works but is no longer required.
```

- [ ] **Step 2: Commit**

```bash
git add NOTES.md
git commit -m "docs(secret): document libsecret secret storage"
```

---

## Final Verification

After all tasks, run the full suite twice and confirm the plugin builds:

```bash
ninja -C build && meson test -C build
OPENAI_API_KEY=dummy MISTRAL_API_KEY=dummy ANTHROPIC_API_KEY=dummy meson test -C build
```

Expected: 13 test suites pass in both runs (added `secret-store`). The
`/secret-store/round-trip` case passes when a secret service is reachable, else
reports SKIP — both are green. Then dispatch a holistic reviewer over
`git diff master...secret-storage` and finish with
**superpowers:finishing-a-development-branch**.

---

## Self-Review (plan vs. spec)

- **Spec coverage:** secret-store module + schema + sync API (T1); `${secret:NAME}`
  interpolation branch + fake-lookup test seam + fallback-to-`""` (T2);
  `collect_secret_refs` pure scan (T3); live re-interpolation trigger via file
  rewrite (T4, justified by the monitor only reacting to CHANGES_DONE_HINT/CREATED);
  prefs-dialog Secrets section with masked entry + Store/Clear (T5); libsecret
  hard dependency + internal (non-installed) header (T1); skippable round-trip
  test (T1); docs + prerequisite update (T6). All spec sections map to a task.
- **Type/signature consistency:** `llm_ghost_secret_lookup/store/clear(name[,value],error)`,
  `LlmGhostSecretLookupFn`/`_llm_ghost_settings_set_secret_lookup_for_testing`,
  `_llm_ghost_settings_collect_secret_refs(JsonObject*)→char**`, and
  `llm_ghost_settings_touch(path,error)` are spelled identically across the
  internal header, the implementation, the tests, and the dialog call sites.
- **Ownership:** `lookup` normalizes to `g_free`; `collect_secret_refs` returns a
  `g_strfreev`-able strv with a `g_free`-keyed hash set freeing scan temporaries;
  the dialog's `SecretRow` is owned by the row box via `g_object_set_data_full`
  (both buttons borrow), entries/labels owned by the widget tree.
- **No placeholders:** every code step shows complete code; every run step states
  the command and expected outcome.
- **Decomposition:** the libsecret edge is isolated in one module; the settings
  changes reuse the existing recursive walk shape; the GTK glue is the only
  untested surface and contains no logic that isn't delegated to a tested
  function.
```
