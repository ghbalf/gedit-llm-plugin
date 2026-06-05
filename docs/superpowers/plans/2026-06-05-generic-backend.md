# Generic (Template) Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a config-driven `LlmGhostGenericBackend` that calls non-OpenAI-shaped LLM APIs (Anthropic native, Gemini native, …) purely from a JSON `settings.json` stanza — a URL, an arbitrary-headers map, a request-body template with `{{prefix}}`/`{{suffix}}`/`{{model}}` placeholders, and a dotted `response_path`.

**Architecture:** Generalize the shared HTTP util to send an arbitrary-headers POST (the Bearer call becomes a thin wrapper over it). Two pure functions do the real work — structural placeholder substitution into a parsed JSON template (parse → deep-copy → walk → re-serialize, so escaping is automatic) and dotted-path response extraction. A new backend ties them together, reusing the shared HTTP transport and a shared single-line cleanup helper extracted from the OpenAI backend. The settings factory grows a `"generic"` branch.

**Tech Stack:** C (gnu11), GLib/GObject, GIO, libsoup-3, json-glib, meson/ninja, GLib `g_test` (display-free unit suite + in-process `SoupServer` loopback).

**Spec:** `docs/superpowers/specs/2026-06-05-generic-backend-design.md`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-http-util.{c,h}` (modify) | Add `_llm_ghost_http_post_json_headers_async` (arbitrary headers); reimplement the Bearer call as a wrapper over it. |
| `lib/llmghost-text-util.{c,h}` (new) | Shared `_llm_ghost_clean_single_line` (fence-strip + first line), moved out of the OpenAI backend. |
| `lib/llmghost-generic-backend.{c,h}` + `-internal.h` (new) | The backend + its two pure functions (`_build_body`, `_extract`) behind the `-internal.h` test seam. |
| `lib/llmghost-backend-factory.c` (modify) | `build_generic` + a `"generic"` dispatch case. |
| `lib/llmghost-openai-backend.c` + `-internal.h` (modify) | Call the shared cleanup; drop the local copy. |
| `tests/test-generic-body.c` (new) | Unit tests for `_build_body` + `_extract`. |
| `tests/test-text-util.c` (new) | Unit tests for the shared cleanup (moved from the OpenAI body test). |
| `tests/test-http-util.c` (modify) | Loopback test asserting custom headers are sent. |
| `tests/test-settings.c` (modify) | Factory GType test for `"generic"`. |
| `examples/anthropic.json`, `examples/gemini.json` (new) | Ready-to-paste templates. |
| `lib/llmghost.h`, `lib/meson.build`, `tests/meson.build`, `NOTES.md` (modify) | Wiring + docs. |

**Conventions (verified in-tree):** pure helpers behind `*-internal.h` (never added to `llmghost_headers`); `#define G_LOG_DOMAIN` at the top of each `.c`; finalize with `g_clear_*`; leak-free json-glib idioms (`json_object_ref` to keep a node alive; `json_node_unref` a generator root after `json_generator_to_data`); the warning-determinism test convention (`g_unsetenv` template vars + `g_test_expect_message`, robust to exported keys and `G_DEBUG=fatal-warnings`).

---

## Task 1: HTTP util — arbitrary-headers POST core + Bearer wrapper

Generalize the shared transport so a backend can send any headers (Anthropic's `x-api-key`/`anthropic-version`), keeping the existing Bearer call behaving identically for ollama/openai/mistral.

**Files:**
- Modify: `lib/llmghost-http-util.h`, `lib/llmghost-http-util.c`
- Test: `tests/test-http-util.c`

- [ ] **Step 1: Declare the new core in `lib/llmghost-http-util.h`**

Add this declaration directly above the existing `_llm_ghost_http_post_json_async` declaration:

```c
/* POST @json_body to @url with arbitrary request @headers (each member of the
 * JsonObject is sent as "name: value"; non-string values are skipped with a
 * warning). Sets "Content-Type: application/json" unless @headers supplies its
 * own "Content-Type". Takes ownership of @json_body (frees it). @session,
 * @cancellable, @headers are borrowed. Finish with the same
 * _llm_ghost_http_post_json_finish() below. */
void       _llm_ghost_http_post_json_headers_async (SoupSession         *session,
                                                    const char          *url,
                                                    JsonObject          *headers,
                                                    char                *json_body,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
```

- [ ] **Step 2: Add a failing custom-headers loopback test to `tests/test-http-util.c`**

The file already has an in-process `SoupServer` (`Srv`/`Captured`/`server_cb`) and a `post()` helper. First extend `Captured` and `server_cb` to capture two custom headers. Change the `Captured` struct (currently `last_auth`/`last_content_type`/`last_body`) to also have:

```c
  char *last_xapikey;       /* captured x-api-key header ("" if none) */
  char *last_anthropic_ver; /* captured anthropic-version header ("" if none) */
```

In `server_cb`, after the existing `auth`/`ct` captures, add:

```c
  const char *xak = soup_message_headers_get_one (h, "x-api-key");
  const char *av  = soup_message_headers_get_one (h, "anthropic-version");
  g_clear_pointer (&cap->last_xapikey, g_free);
  g_clear_pointer (&cap->last_anthropic_ver, g_free);
  cap->last_xapikey      = g_strdup (xak ? xak : "");
  cap->last_anthropic_ver = g_strdup (av ? av : "");
```

In `srv_free`, free the two new fields:

```c
  g_free (s->cap.last_xapikey);
  g_free (s->cap.last_anthropic_ver);
```

Add a driver that posts via the new headers function (mirrors the existing `post()` but takes a `JsonObject *headers`), then the test:

```c
static JsonNode *
post_headers (Srv *s, const char *path, JsonObject *headers, const char *body, GError **error)
{
  SoupSession *session = soup_session_new ();
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  Wait w = { .loop = loop };
  char *url = g_strconcat (s->base, path + 1, NULL);

  _llm_ghost_http_post_json_headers_async (session, url, headers, g_strdup (body),
                                           NULL, on_done, &w);
  g_main_loop_run (loop);

  g_free (url);
  g_main_loop_unref (loop);
  g_object_unref (session);
  if (error != NULL) *error = w.error; else g_clear_error (&w.error);
  return w.node;
}

static void
test_custom_headers (void)
{
  Srv *s = srv_new ();
  JsonObject *headers = json_object_new ();
  json_object_set_string_member (headers, "x-api-key", "secret-key");
  json_object_set_string_member (headers, "anthropic-version", "2023-06-01");

  JsonNode *node = post_headers (s, "/ok", headers, "{\"x\":1}", NULL);
  g_assert_nonnull (node);

  g_assert_cmpstr (s->cap.last_xapikey,       ==, "secret-key");
  g_assert_cmpstr (s->cap.last_anthropic_ver, ==, "2023-06-01");
  g_assert_cmpstr (s->cap.last_auth,          ==, "");                 /* no Bearer */
  g_assert_true (g_str_has_prefix (s->cap.last_content_type, "application/json"));

  json_node_unref (node);
  json_object_unref (headers);
  srv_free (s);
}

static void
test_bearer_wrapper_still_works (void)
{
  /* The reimplemented Bearer call must still send Authorization through the
   * new core. */
  Srv *s = srv_new ();
  JsonNode *node = post (s, "/ok", "tok", "{}", NULL);
  g_assert_nonnull (node);
  g_assert_cmpstr (s->cap.last_auth, ==, "Bearer tok");
  json_node_unref (node);
  srv_free (s);
}
```

Register both in `main`:

```c
  g_test_add_func ("/http-util/custom-headers",  test_custom_headers);
  g_test_add_func ("/http-util/bearer-wrapper",  test_bearer_wrapper_still_works);
```

- [ ] **Step 3: Run to verify it fails to link**

Run: `ninja -C build`
Expected: FAIL — undefined reference to `_llm_ghost_http_post_json_headers_async`.

- [ ] **Step 4: Implement the new core and rewrite the Bearer call in `lib/llmghost-http-util.c`**

Replace the entire existing `_llm_ghost_http_post_json_async` function (lines 61-99) with the new core plus a thin Bearer wrapper. Leave `on_soup_response` and `_llm_ghost_http_post_json_finish` unchanged.

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

  SoupMessage *msg = soup_message_new (SOUP_METHOD_POST, url);
  if (msg == NULL)
    {
      g_free (json_body);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                               "invalid URL: %s", url ? url : "(null)");
      g_object_unref (task);
      return;
    }

  /* Apply caller headers. A caller-supplied Content-Type wins over the JSON
   * default; we route it through set_request_body_from_bytes (below) rather
   * than appending, so it is never duplicated. */
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
              content_type = json_node_get_string (val);   /* borrowed; used now */
              continue;
            }
          soup_message_headers_append (h, name, json_node_get_string (val));
        }
    }

  GBytes *bytes = g_bytes_new_take (json_body, strlen (json_body));
  soup_message_set_request_body_from_bytes (msg, content_type, bytes);
  g_bytes_unref (bytes);

  /* Keep the SoupMessage alive until the handler reads its status. */
  g_task_set_task_data (task, msg, g_object_unref);

  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_soup_response, task);
}

void
_llm_ghost_http_post_json_async (SoupSession         *session,
                                 const char          *url,
                                 const char          *bearer,
                                 char                *json_body,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  JsonObject *headers = NULL;
  if (bearer != NULL && *bearer != '\0')
    {
      char *auth = g_strdup_printf ("Bearer %s", bearer);
      headers = json_object_new ();
      json_object_set_string_member (headers, "Authorization", auth);
      g_free (auth);
    }

  /* The core consumes @json_body and reads @headers synchronously before the
   * async send returns, so unref-ing headers right after the call is safe. */
  _llm_ghost_http_post_json_headers_async (session, url, headers, json_body,
                                           cancellable, callback, user_data);
  if (headers != NULL)
    json_object_unref (headers);
}
```

- [ ] **Step 5: Build and run the http-util test**

Run:
```bash
ninja -C build && meson test -C build --suite unit http-util -v
```
Expected: PASS — the existing 4 cases plus `/http-util/custom-headers` and `/http-util/bearer-wrapper`.

- [ ] **Step 6: Run the full suite (the 3 existing backends use the Bearer wrapper)**

Run: `meson test -C build`
Expected: all 10 suites PASS (ollama/openai/mistral still work through the reimplemented Bearer call).

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-http-util.h lib/llmghost-http-util.c tests/test-http-util.c
git commit -m "feat(http-util): arbitrary-headers POST core; Bearer call becomes a wrapper"
```

---

## Task 2: Shared single-line cleanup helper

Move the OpenAI chat-cleanup into a shared internal helper both backends call.

**Files:**
- Create: `lib/llmghost-text-util.h`, `lib/llmghost-text-util.c`, `tests/test-text-util.c`
- Modify: `lib/llmghost-openai-backend-internal.h`, `lib/llmghost-openai-backend.c`, `tests/test-openai-body.c`, `lib/meson.build`, `tests/meson.build`

- [ ] **Step 1: Create `lib/llmghost-text-util.h`**

```c
#pragma once

/* Internal (NOT installed) shared text helpers for the chat-style backends. */

#include <glib.h>

G_BEGIN_DECLS

/* Trim, unwrap a single leading ``` fence (with optional language tag) and its
 * trailing ```, then truncate at the first newline. NULL-safe; always returns a
 * newly-allocated string (possibly ""). Turns chat-model prose into ghost text. */
char *_llm_ghost_clean_single_line (const char *raw);

G_END_DECLS
```

- [ ] **Step 2: Create `tests/test-text-util.c` (the cleanup cases, repointed to the shared name)**

```c
#include <glib.h>
#include "llmghost-text-util.h"

static void
check_clean (const char *raw, const char *expect)
{
  char *got = _llm_ghost_clean_single_line (raw);
  g_assert_cmpstr (got, ==, expect);
  g_free (got);
}

static void
test_clean_single_line (void)
{
  check_clean ("abc", "abc");
  check_clean ("  abc  ", "abc");
  check_clean (NULL, "");
  check_clean ("", "");
  check_clean ("foo()\nbar()", "foo()");                    /* truncate at newline */
  check_clean ("```\nfoo()\n```", "foo()");                 /* bare fence */
  check_clean ("```c\nfoo()\n```", "foo()");                /* lang-tagged fence */
  check_clean ("```python\nx = 1\ny = 2\n```", "x = 1");    /* fence + truncate */
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/text-util/clean-single-line", test_clean_single_line);
  return g_test_run ();
}
```

- [ ] **Step 3: Create `lib/llmghost-text-util.c` (the moved cleanup body)**

```c
#define G_LOG_DOMAIN "llmghost-text-util"

#include "llmghost-text-util.h"

#include <string.h>

char *
_llm_ghost_clean_single_line (const char *raw)
{
  if (raw == NULL)
    return g_strdup ("");

  char *trimmed = g_strdup (raw);
  g_strstrip (trimmed);                 /* trims leading + trailing whitespace */

  char *unfenced = trimmed;             /* may be reassigned to a new alloc */
  if (g_str_has_prefix (trimmed, "```"))
    {
      const char *nl = strchr (trimmed, '\n');
      if (nl != NULL)
        {
          const char *inner = nl + 1;
          char *close = g_strrstr (inner, "```");
          unfenced = close != NULL
                       ? g_strndup (inner, (gsize) (close - inner))
                       : g_strdup (inner);
          g_strstrip (unfenced);
        }
    }

  const char *nl2 = strchr (unfenced, '\n');
  char *result = nl2 != NULL
                   ? g_strndup (unfenced, (gsize) (nl2 - unfenced))
                   : g_strdup (unfenced);

  if (unfenced != trimmed)
    g_free (unfenced);
  g_free (trimmed);
  return result;
}
```

- [ ] **Step 4: Repoint the OpenAI backend to the shared helper**

In `lib/llmghost-openai-backend-internal.h`, DELETE the `_llm_ghost_openai_clean_chat_completion` declaration (the comment block + the line):

```c
/* Trim, unwrap a single ``` fence, then truncate at the first newline.
 * NULL-safe; always returns a newly-allocated string (possibly ""). */
char *_llm_ghost_openai_clean_chat_completion  (const char *raw);
```

In `lib/llmghost-openai-backend.c`:
1. Add `#include "llmghost-text-util.h"` next to the other includes.
2. DELETE the entire `_llm_ghost_openai_clean_chat_completion` function definition (the `/* ---- response cleanup ---- */` section, lines ~111-146).
3. Find its call site (around line 207, inside `_llm_ghost_openai_extract_completion`): replace
   `return _llm_ghost_openai_clean_chat_completion (content);`
   with
   `return _llm_ghost_clean_single_line (content);`

- [ ] **Step 5: Repoint the OpenAI test**

In `tests/test-openai-body.c`:
1. Add `#include "llmghost-text-util.h"` to the includes.
2. DELETE the `check_clean` helper (line ~73-78) and the `test_clean_chat_completion` function (line ~81-91) — these moved to `test-text-util.c`.
3. DELETE its registration line: `g_test_add_func ("/openai-body/clean", test_clean_chat_completion);`
4. Leave `test_extract_chat_cleans` and everything else intact — it exercises the cleanup through `_llm_ghost_openai_extract_completion`, which now calls the shared helper.

- [ ] **Step 6: Wire the new files into meson**

In `lib/meson.build`: add `'llmghost-text-util.c',` to `llmghost_sources` and `'llmghost-text-util.h',` to `llmghost_headers`.

In `tests/meson.build`, append after the `test-mistral-body` block:

```meson
test_text_util = executable(
  'test-text-util',
  'test-text-util.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('text-util', test_text_util, suite: 'unit')
```

- [ ] **Step 7: Build and run the affected tests**

Run:
```bash
ninja -C build && meson test -C build --suite unit text-util openai-body -v
```
Expected: PASS — `text-util` (the moved clean cases) and `openai-body` (still green; extraction still cleans). Then `meson test -C build` → all pass.

- [ ] **Step 8: Commit**

```bash
git add lib/llmghost-text-util.h lib/llmghost-text-util.c tests/test-text-util.c \
        lib/llmghost-openai-backend-internal.h lib/llmghost-openai-backend.c \
        tests/test-openai-body.c lib/meson.build tests/meson.build
git commit -m "refactor(text-util): extract shared single-line chat cleanup"
```

---

## Task 3: Generic backend — template substitution (`_build_body`)

The first pure function: fill the request-body template with the FIM context, structurally (parse → deep-copy → walk → re-serialize).

**Files:**
- Create: `lib/llmghost-generic-backend-internal.h`, `lib/llmghost-generic-backend.c`, `tests/test-generic-body.c`
- Modify: `lib/meson.build`, `tests/meson.build`

- [ ] **Step 1: Create `lib/llmghost-generic-backend-internal.h`**

```c
#pragma once

/* Testing-only internal API for the generic backend. NOT installed. Pure
 * template-substitution and response-path extraction for direct unit testing. */

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Deep-copy @template and replace the tokens {{prefix}}, {{suffix}}, {{model}}
 * inside every string value (objects and arrays, recursively). Each string is
 * scanned once left-to-right, so substituted content is never re-scanned (a
 * prefix containing "{{suffix}}" is inserted verbatim). Unknown {{tokens}} are
 * left as-is. NULL placeholder values are treated as "". Returns a
 * newly-allocated serialized JSON string. @template is not modified. */
char *_llm_ghost_generic_build_body (JsonObject *template,
                                     const char *prefix,
                                     const char *suffix,
                                     const char *model);

G_END_DECLS
```

- [ ] **Step 2: Create `tests/test-generic-body.c` with failing substitution tests**

```c
#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-generic-backend-internal.h"

/* Parse a template literal into a JsonObject (caller unrefs). */
static JsonObject *
obj_from (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonObject *obj = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  return obj;
}

/* Build a body, then parse the result back for assertions (caller unrefs). */
static JsonObject *
build_and_parse (JsonObject *tmpl, const char *prefix, const char *suffix, const char *model)
{
  char *body = _llm_ghost_generic_build_body (tmpl, prefix, suffix, model);
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, body, -1, &error));   /* valid JSON */
  g_assert_no_error (error);
  JsonObject *out = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  g_free (body);
  return out;
}

static void
test_build_simple (void)
{
  JsonObject *t = obj_from ("{\"p\":\"{{prefix}}\",\"s\":\"{{suffix}}\"}");
  JsonObject *o = build_and_parse (t, "AAA", "BBB", NULL);
  g_assert_cmpstr (json_object_get_string_member (o, "p"), ==, "AAA");
  g_assert_cmpstr (json_object_get_string_member (o, "s"), ==, "BBB");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_model (void)
{
  JsonObject *t = obj_from ("{\"m\":\"{{model}}\"}");
  JsonObject *o = build_and_parse (t, "", "", "claude-x");
  g_assert_cmpstr (json_object_get_string_member (o, "m"), ==, "claude-x");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_escaping (void)
{
  /* A prefix with a quote, newline, and backslash must round-trip exactly —
   * proof that we substitute structurally and let json-glib do the escaping. */
  JsonObject *t = obj_from ("{\"p\":\"{{prefix}}\"}");
  JsonObject *o = build_and_parse (t, "a\"b\nc\\d", "", NULL);
  g_assert_cmpstr (json_object_get_string_member (o, "p"), ==, "a\"b\nc\\d");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_nested (void)
{
  JsonObject *t = obj_from (
    "{\"messages\":[{\"role\":\"user\",\"content\":\"X{{prefix}}Y\"}]}");
  JsonObject *o = build_and_parse (t, "P", "", NULL);
  JsonArray *msgs = json_object_get_array_member (o, "messages");
  JsonObject *m0 = json_array_get_object_element (msgs, 0);
  g_assert_cmpstr (json_object_get_string_member (m0, "content"), ==, "XPY");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_unknown_placeholder_verbatim (void)
{
  JsonObject *t = obj_from ("{\"k\":\"{{bogus}}\"}");
  JsonObject *o = build_and_parse (t, "P", "S", "M");
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "{{bogus}}");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_single_pass_safety (void)
{
  /* prefix value contains "{{suffix}}" — it must NOT be re-substituted. */
  JsonObject *t = obj_from ("{\"k\":\"{{prefix}}\"}");
  JsonObject *o = build_and_parse (t, "a{{suffix}}b", "S", NULL);
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "a{{suffix}}b");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_no_placeholders (void)
{
  JsonObject *t = obj_from ("{\"k\":\"v\",\"n\":5}");
  JsonObject *o = build_and_parse (t, "P", "S", "M");
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "v");
  g_assert_cmpint (json_object_get_int_member (o, "n"), ==, 5);
  json_object_unref (o);
  json_object_unref (t);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/generic-body/simple",            test_build_simple);
  g_test_add_func ("/generic-body/model",             test_build_model);
  g_test_add_func ("/generic-body/escaping",          test_build_escaping);
  g_test_add_func ("/generic-body/nested",            test_build_nested);
  g_test_add_func ("/generic-body/unknown-verbatim",  test_build_unknown_placeholder_verbatim);
  g_test_add_func ("/generic-body/single-pass-safety", test_build_single_pass_safety);
  g_test_add_func ("/generic-body/no-placeholders",   test_build_no_placeholders);
  return g_test_run ();
}
```

- [ ] **Step 3: Create `lib/llmghost-generic-backend.c` with the substitution functions only**

(The GObject is added in Task 5; this task keeps the file to the pure builder so the lib compiles and the unit test links.)

```c
#define G_LOG_DOMAIN "llmghost-generic"

#include "llmghost-generic-backend-internal.h"

#include <string.h>

/* ---- template substitution --------------------------------------------- */

/* Replace {{prefix}}/{{suffix}}/{{model}} in @in. Single left-to-right pass;
 * inserted values are never re-scanned. Unknown {{tokens}} copied verbatim.
 * NULL values count as "". Newly-allocated. */
static char *
substitute (const char *in, const char *prefix, const char *suffix, const char *model)
{
  GString *out = g_string_new (NULL);
  const char *p = in;
  while (*p != '\0')
    {
      if (p[0] == '{' && p[1] == '{')
        {
          const char *end = strstr (p + 2, "}}");
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              const char *val = NULL;
              gboolean known = TRUE;
              if (strcmp (name, "prefix") == 0)      val = prefix;
              else if (strcmp (name, "suffix") == 0) val = suffix;
              else if (strcmp (name, "model") == 0)  val = model;
              else                                   known = FALSE;
              g_free (name);
              if (known)
                {
                  g_string_append (out, val != NULL ? val : "");
                  p = end + 2;
                  continue;
                }
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

/* Recurse @node, replacing every string value in place. Handles object members
 * and array elements uniformly via json_node_set_string (json-glib has no
 * array-element setter, so we mutate the element node directly). */
static void
substitute_node (JsonNode *node, const char *prefix, const char *suffix, const char *model)
{
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      JsonObject *obj = json_node_get_object (node);
      GList *members = json_object_get_members (obj);
      for (GList *l = members; l != NULL; l = l->next)
        substitute_node (json_object_get_member (obj, l->data), prefix, suffix, model);
      g_list_free (members);
    }
  else if (JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *arr = json_node_get_array (node);
      guint n = json_array_get_length (arr);
      for (guint i = 0; i < n; i++)
        substitute_node (json_array_get_element (arr, i), prefix, suffix, model);
    }
  else if (JSON_NODE_HOLDS_VALUE (node) &&
           json_node_get_value_type (node) == G_TYPE_STRING)
    {
      char *sub = substitute (json_node_get_string (node), prefix, suffix, model);
      json_node_set_string (node, sub);
      g_free (sub);
    }
}

char *
_llm_ghost_generic_build_body (JsonObject *template,
                               const char *prefix,
                               const char *suffix,
                               const char *model)
{
  /* Deep-copy so the stored template is never mutated. */
  JsonNode *wrap = json_node_alloc ();
  json_node_init_object (wrap, template);   /* refs template */
  JsonNode *copy = json_node_copy (wrap);   /* deep copy */
  json_node_unref (wrap);

  substitute_node (copy, prefix, suffix, model);

  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, copy);      /* transfer none */
  char *out = json_generator_to_data (gen, NULL);
  g_object_unref (gen);
  json_node_unref (copy);
  return out;
}
```

- [ ] **Step 4: Wire into meson**

In `lib/meson.build`: add `'llmghost-generic-backend.c',` to `llmghost_sources` (do NOT add any header yet — the public `.h` arrives in Task 5).

In `tests/meson.build`, append:

```meson
test_generic_body = executable(
  'test-generic-body',
  'test-generic-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('generic-body', test_generic_body, suite: 'unit')
```

- [ ] **Step 5: Build and run**

Run:
```bash
ninja -C build && meson test -C build --suite unit generic-body -v
```
Expected: PASS — all 7 `/generic-body/*` substitution cases.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-generic-backend-internal.h lib/llmghost-generic-backend.c \
        tests/test-generic-body.c lib/meson.build tests/meson.build
git commit -m "feat(generic): structural template substitution (build_body)"
```

---

## Task 4: Generic backend — response-path extraction (`_extract`)

The second pure function: pull the completion string out of a parsed response by a dotted path.

**Files:**
- Modify: `lib/llmghost-generic-backend-internal.h`, `lib/llmghost-generic-backend.c`, `tests/test-generic-body.c`

- [ ] **Step 1: Declare `_extract` in `lib/llmghost-generic-backend-internal.h`**

Add before `G_END_DECLS`:

```c
/* Walk a dotted @path through @root. An all-digits segment indexes an array;
 * any other segment selects an object member. Returns the located string
 * (newly-allocated), or NULL + @error (G_IO_ERROR_FAILED, message naming the
 * failing segment) when the path does not resolve to a string. */
char *_llm_ghost_generic_extract (JsonNode  *root,
                                  const char *path,
                                  GError    **error);
```

Add the GIO include at the top of the header (for `G_IO_ERROR`): change `#include <glib.h>` to also have `#include <gio/gio.h>`.

- [ ] **Step 2: Add failing extraction tests to `tests/test-generic-body.c`**

Add `#include <gio/gio.h>` to the includes. Add a parse-node helper and the tests, then register them:

```c
static JsonNode *
node_from (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonNode *node = json_node_copy (json_parser_get_root (parser));
  g_object_unref (parser);
  return node;
}

static void
test_extract_object_leaf (void)
{
  JsonNode *n = node_from ("{\"a\":\"hi\"}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "a", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (r, ==, "hi");
  g_free (r);
  json_node_unref (n);
}

static void
test_extract_array_index (void)
{
  JsonNode *n = node_from ("{\"c\":[\"x\",\"y\"]}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "c.1", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (r, ==, "y");
  g_free (r);
  json_node_unref (n);
}

static void
test_extract_anthropic_path (void)
{
  JsonNode *n = node_from ("{\"content\":[{\"type\":\"text\",\"text\":\"done\"}]}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "content.0.text", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (r, ==, "done");
  g_free (r);
  json_node_unref (n);
}

static void
test_extract_gemini_path (void)
{
  JsonNode *n = node_from (
    "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"g\"}]}}]}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "candidates.0.content.parts.0.text", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (r, ==, "g");
  g_free (r);
  json_node_unref (n);
}

static void
test_extract_missing_member (void)
{
  JsonNode *n = node_from ("{\"a\":{}}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "a.b", &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  json_node_unref (n);
}

static void
test_extract_index_out_of_range (void)
{
  JsonNode *n = node_from ("{\"c\":[]}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "c.0", &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  json_node_unref (n);
}

static void
test_extract_index_into_object (void)
{
  JsonNode *n = node_from ("{\"a\":{}}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "a.0", &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  json_node_unref (n);
}

static void
test_extract_member_of_array (void)
{
  JsonNode *n = node_from ("{\"c\":[]}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "c.x", &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  json_node_unref (n);
}

static void
test_extract_non_string_leaf (void)
{
  JsonNode *n = node_from ("{\"a\":5}");
  GError *error = NULL;
  char *r = _llm_ghost_generic_extract (n, "a", &error);
  g_assert_null (r);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  json_node_unref (n);
}
```

Register in `main`:

```c
  g_test_add_func ("/generic-body/extract-object",      test_extract_object_leaf);
  g_test_add_func ("/generic-body/extract-array",       test_extract_array_index);
  g_test_add_func ("/generic-body/extract-anthropic",   test_extract_anthropic_path);
  g_test_add_func ("/generic-body/extract-gemini",      test_extract_gemini_path);
  g_test_add_func ("/generic-body/extract-missing",     test_extract_missing_member);
  g_test_add_func ("/generic-body/extract-oob",         test_extract_index_out_of_range);
  g_test_add_func ("/generic-body/extract-index-obj",   test_extract_index_into_object);
  g_test_add_func ("/generic-body/extract-member-arr",  test_extract_member_of_array);
  g_test_add_func ("/generic-body/extract-nonstring",   test_extract_non_string_leaf);
```

- [ ] **Step 3: Run to verify the new tests fail to link**

Run: `ninja -C build`
Expected: FAIL — undefined reference to `_llm_ghost_generic_extract`.

- [ ] **Step 4: Implement `_llm_ghost_generic_extract` in `lib/llmghost-generic-backend.c`**

Add `#include <gio/gio.h>` at the top (after `#include <string.h>`), then add this function after `_llm_ghost_generic_build_body`:

```c
/* ---- response-path extraction ------------------------------------------ */

static gboolean
seg_is_index (const char *seg)
{
  if (*seg == '\0')
    return FALSE;
  for (const char *p = seg; *p != '\0'; p++)
    if (!g_ascii_isdigit (*p))
      return FALSE;
  return TRUE;
}

char *
_llm_ghost_generic_extract (JsonNode *root, const char *path, GError **error)
{
  if (root == NULL || path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: null response or response_path");
      return NULL;
    }

  char **segs = g_strsplit (path, ".", -1);
  JsonNode *cur = root;

  for (int i = 0; segs[i] != NULL; i++)
    {
      const char *seg = segs[i];
      if (seg_is_index (seg))
        {
          if (!JSON_NODE_HOLDS_ARRAY (cur))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path segment \"%s\" expected an array", seg);
              g_strfreev (segs);
              return NULL;
            }
          JsonArray *arr = json_node_get_array (cur);
          guint64 idx = g_ascii_strtoull (seg, NULL, 10);
          if (idx >= json_array_get_length (arr))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path index %s out of range", seg);
              g_strfreev (segs);
              return NULL;
            }
          cur = json_array_get_element (arr, (guint) idx);
        }
      else
        {
          if (!JSON_NODE_HOLDS_OBJECT (cur))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path segment \"%s\" expected an object", seg);
              g_strfreev (segs);
              return NULL;
            }
          JsonObject *obj = json_node_get_object (cur);
          if (!json_object_has_member (obj, seg))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path member \"%s\" not found", seg);
              g_strfreev (segs);
              return NULL;
            }
          cur = json_object_get_member (obj, seg);
        }
    }

  g_strfreev (segs);

  if (cur == NULL || !JSON_NODE_HOLDS_VALUE (cur) ||
      json_node_get_value_type (cur) != G_TYPE_STRING)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "generic: response_path did not resolve to a string");
      return NULL;
    }

  return g_strdup (json_node_get_string (cur));
}
```

- [ ] **Step 5: Build and run**

Run:
```bash
ninja -C build && meson test -C build --suite unit generic-body -v
```
Expected: PASS — the 7 substitution cases plus the 9 extraction cases (16 total).

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-generic-backend-internal.h lib/llmghost-generic-backend.c tests/test-generic-body.c
git commit -m "feat(generic): dotted-path response extraction"
```

---

## Task 5: `LlmGhostGenericBackend` GObject

Tie the two pure functions, the shared cleanup, and the headers transport into a backend implementing `LlmGhostBackend`.

**Files:**
- Create: `lib/llmghost-generic-backend.h`
- Modify: `lib/llmghost-generic-backend.c`, `lib/llmghost.h`, `lib/meson.build`

This unit's request flow has no display surface and is exercised end-to-end by the factory GType test (Task 6) and indirectly by the pure-function tests; a full live HTTP round-trip against a real provider is the only manual step. (The transport itself is already loopback-tested in Task 1.)

- [ ] **Step 1: Create the public header `lib/llmghost-generic-backend.h`**

```c
#pragma once

#include "llmghost-backend.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_GENERIC_BACKEND (llm_ghost_generic_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostGenericBackend, llm_ghost_generic_backend,
                      LLM_GHOST, GENERIC_BACKEND, GObject)

/**
 * llm_ghost_generic_backend_new:
 * @url:              endpoint URL (may already contain an interpolated key).
 * @headers:          request headers (string→string), or %NULL. Refs it.
 * @model:            value substituted for {{model}}, or %NULL.
 * @request_template: JSON body template with {{prefix}}/{{suffix}}/{{model}}. Refs it.
 * @response_path:    dotted path to the completion string in the response.
 *
 * A config-driven backend for non-OpenAI-shaped JSON-over-POST APIs. Holds an
 * owning ref on @headers and @request_template so it is independent of the
 * settings object's lifetime (and a live reload). Missing required fields are
 * logged; requests then fail gracefully.
 */
LlmGhostBackend *llm_ghost_generic_backend_new (const char *url,
                                                JsonObject *headers,
                                                const char *model,
                                                JsonObject *request_template,
                                                const char *response_path);

G_END_DECLS
```

- [ ] **Step 2: Append the GObject implementation to `lib/llmghost-generic-backend.c`**

Add these includes at the top (next to the existing ones): `#include "llmghost-generic-backend.h"`, `#include "llmghost-text-util.h"`, `#include "llmghost-http-util.h"`, `#include <libsoup/soup.h>`. Then append the type below the two pure functions:

```c
/* ---- type --------------------------------------------------------------- */

#define REQUEST_TIMEOUT_SEC 30

struct _LlmGhostGenericBackend
{
  GObject      parent_instance;

  SoupSession *session;
  char        *url;
  JsonObject  *headers;            /* owned ref, or NULL */
  char        *model;              /* or NULL */
  JsonObject  *request_template;   /* owned ref, or NULL */
  char        *response_path;
};

static void llm_ghost_generic_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostGenericBackend, llm_ghost_generic_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_generic_backend_iface_init))

/* ---- request flow ------------------------------------------------------- */

static void
on_http_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  GTask  *task  = G_TASK (user_data);
  GError *error = NULL;

  JsonNode *root = _llm_ghost_http_post_json_finish (result, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  LlmGhostGenericBackend *self = g_task_get_source_object (task);
  char *raw = _llm_ghost_generic_extract (root, self->response_path, &error);
  json_node_unref (root);
  if (raw == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  char *clean = _llm_ghost_clean_single_line (raw);
  g_free (raw);
  g_task_return_pointer (task, clean, g_free);
  g_object_unref (task);
}

static void
generic_request (LlmGhostBackend     *backend,
                 const char          *prefix,
                 const char          *suffix,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
  LlmGhostGenericBackend *self = LLM_GHOST_GENERIC_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  if (self->request_template == NULL || self->url == NULL || *self->url == '\0' ||
      self->response_path == NULL || *self->response_path == '\0')
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "generic backend: incomplete configuration");
      g_object_unref (task);
      return;
    }

  char *body = _llm_ghost_generic_build_body (self->request_template,
                                              prefix, suffix, self->model);
  _llm_ghost_http_post_json_headers_async (self->session, self->url, self->headers,
                                           body, cancellable, on_http_done, task);
}

static char *
generic_request_finish (LlmGhostBackend *backend, GAsyncResult *result, GError **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_generic_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = generic_request;
  iface->request_finish = generic_request_finish;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llm_ghost_generic_backend_finalize (GObject *object)
{
  LlmGhostGenericBackend *self = LLM_GHOST_GENERIC_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->url, g_free);
  g_clear_pointer (&self->headers, json_object_unref);
  g_clear_pointer (&self->model, g_free);
  g_clear_pointer (&self->request_template, json_object_unref);
  g_clear_pointer (&self->response_path, g_free);
  G_OBJECT_CLASS (llm_ghost_generic_backend_parent_class)->finalize (object);
}

static void
llm_ghost_generic_backend_class_init (LlmGhostGenericBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_generic_backend_finalize;
}

static void
llm_ghost_generic_backend_init (LlmGhostGenericBackend *self)
{
  self->session = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
}

/* ---- construction ------------------------------------------------------- */

LlmGhostBackend *
llm_ghost_generic_backend_new (const char *url,
                               JsonObject *headers,
                               const char *model,
                               JsonObject *request_template,
                               const char *response_path)
{
  LlmGhostGenericBackend *self = g_object_new (LLM_GHOST_TYPE_GENERIC_BACKEND, NULL);

  self->url              = g_strdup (url);
  self->headers          = headers          ? json_object_ref (headers)          : NULL;
  self->model            = g_strdup (model);
  self->request_template = request_template ? json_object_ref (request_template) : NULL;
  self->response_path    = g_strdup (response_path);

  if (url == NULL || *url == '\0')
    g_warning ("generic backend: no \"url\" configured; requests will fail");
  if (request_template == NULL)
    g_warning ("generic backend: no \"request_template\" configured; requests will fail");
  if (response_path == NULL || *response_path == '\0')
    g_warning ("generic backend: no \"response_path\" configured; requests will fail");

  return LLM_GHOST_BACKEND (self);
}
```

- [ ] **Step 3: Add the public header to the umbrella and installed headers**

In `lib/llmghost.h`, add `#include "llmghost-generic-backend.h"` after the other backend includes.

In `lib/meson.build`, add `'llmghost-generic-backend.h',` to `llmghost_headers`.

- [ ] **Step 4: Build and run the full suite**

Run:
```bash
ninja -C build && meson test -C build
```
Expected: everything builds (the new GObject links against http-util + text-util) and all suites pass. No new test here — the backend is covered by the factory GType test in Task 6 and the pure-function tests; this step confirms it compiles and nothing regressed.

- [ ] **Step 5: Commit**

```bash
git add lib/llmghost-generic-backend.h lib/llmghost-generic-backend.c lib/llmghost.h lib/meson.build
git commit -m "feat(generic): LlmGhostGenericBackend (interface impl + request flow)"
```

---

## Task 6: Factory wiring — `build_generic` + `"generic"` dispatch

Make `"backend": "generic"` build the new backend from its stanza.

**Files:**
- Modify: `lib/llmghost-backend-factory.c`, `tests/test-settings.c`

- [ ] **Step 1: Add a failing factory GType test to `tests/test-settings.c`**

Add `#include "llmghost-generic-backend.h"` to the includes. Add the test and register it (after the existing `/settings/factory/*` cases):

```c
static void
test_factory_generic (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"generic\","
    "\"backends\":{\"generic\":{"
      "\"url\":\"http://x/v1\","
      "\"headers\":{\"x-api-key\":\"k\"},"
      "\"model\":\"m\","
      "\"request_template\":{\"messages\":[{\"content\":\"{{prefix}}\"}]},"
      "\"response_path\":\"content.0.text\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_GENERIC_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}
```

Register in `main`:

```c
  g_test_add_func ("/settings/factory/generic",       test_factory_generic);
```

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C build`
Expected: FAIL — `LLM_GHOST_IS_GENERIC_BACKEND` resolves (header is in umbrella), but the factory has no `"generic"` branch, so the built backend is an ollama fallback and the assertion fails (and a `*unknown backend*` warning is emitted). It must build first; if it builds, the test FAILS the GType assertion.

- [ ] **Step 3: Add `build_generic` and the dispatch case in `lib/llmghost-backend-factory.c`**

Add `#include "llmghost-generic-backend.h"` next to the other backend includes. Add a params-object helper and `build_generic` next to the other `build_*` functions:

```c
static JsonObject *
param_object (JsonObject *p, const char *key)
{
  if (p == NULL || !json_object_has_member (p, key))
    return NULL;
  JsonNode *n = json_object_get_member (p, key);
  return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static LlmGhostBackend *
build_generic (JsonObject *p)
{
  return llm_ghost_generic_backend_new (param_string (p, "url"),
                                        param_object (p, "headers"),
                                        param_string (p, "model"),
                                        param_object (p, "request_template"),
                                        param_string (p, "response_path"));
}
```

In `llm_ghost_backend_new_from_settings`, add the dispatch case alongside the others (before the ollama fallback):

```c
  if (g_strcmp0 (which, "generic") == 0)
    return build_generic (p);
```

- [ ] **Step 4: Build and run**

Run:
```bash
ninja -C build && meson test -C build --suite unit settings -v
```
Expected: PASS — including `/settings/factory/generic`. Then run with keys exported to confirm robustness:
```bash
OPENAI_API_KEY=dummy MISTRAL_API_KEY=dummy ANTHROPIC_API_KEY=dummy meson test -C build --suite unit
```
Expected: still PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/llmghost-backend-factory.c tests/test-settings.c
git commit -m "feat(generic): factory builds the generic backend from settings"
```

---

## Task 7: Example templates + documentation

Ship working Anthropic + Gemini templates and document the feature.

**Files:**
- Create: `examples/anthropic.json`, `examples/gemini.json`
- Modify: `NOTES.md`

- [ ] **Step 1: Create `examples/anthropic.json`**

```json
{
  "_help": "Paste the \"generic\" object into your settings.json backends map and set \"backend\": \"generic\". Export ANTHROPIC_API_KEY.",
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
            "content": "You are a code completion engine. Continue the code at the cursor. Output only the raw completion text — no prose, no markdown, no code fences.\n<before_cursor>{{prefix}}</before_cursor>\n<after_cursor>{{suffix}}</after_cursor>" }
        ]
      },
      "response_path": "content.0.text"
    }
  }
}
```

- [ ] **Step 2: Create `examples/gemini.json`**

```json
{
  "_help": "Paste the \"generic\" object into your settings.json backends map and set \"backend\": \"generic\". Export GEMINI_API_KEY. Gemini takes the key in the URL query string.",
  "backend": "generic",
  "backends": {
    "generic": {
      "url": "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=${GEMINI_API_KEY}",
      "headers": {},
      "model": "gemini-2.0-flash",
      "request_template": {
        "contents": [
          { "role": "user",
            "parts": [ { "text": "You are a code completion engine. Continue the code at the cursor. Output only the raw completion text — no prose, no markdown, no code fences.\n<before_cursor>{{prefix}}</before_cursor>\n<after_cursor>{{suffix}}</after_cursor>" } ] }
        ],
        "generationConfig": { "maxOutputTokens": 64 }
      },
      "response_path": "candidates.0.content.parts.0.text"
    }
  }
}
```

- [ ] **Step 3: Document the generic backend in `NOTES.md`**

Open `NOTES.md`. After the "Settings layer (landed)" section added in the settings work, insert a new section:

```markdown
### Generic (template) backend (landed)

For **non-OpenAI-shaped** APIs (Anthropic native, Gemini native, …), set
`"backend": "generic"` and a `backends.generic` stanza with `url`, a `headers`
map, a `model`, a `request_template` (the JSON body with `{{prefix}}`,
`{{suffix}}`, `{{model}}` placeholders), and a dotted `response_path`
(`content.0.text`, `candidates.0.content.parts.0.text`, …). Placeholders are
substituted **structurally** (the template is parsed, the placeholders replaced
inside string values, then re-serialized), so quotes/newlines/backslashes in the
code context are escaped automatically. `${ENV}` interpolation (settings) and
`{{…}}` (per request) are separate phases, so secrets stay in the environment;
an API key that goes in the URL query string (Gemini) is just
`"url": "…?key=${GEMINI_API_KEY}"`. The response is run through the same
single-line/fence-strip cleanup as the OpenAI chat mode. Ready-to-paste
templates live in `examples/anthropic.json` and `examples/gemini.json`.

This does not replace the OpenAI-compat backend, which remains the better path
for OpenAI-*shaped* providers (native FIM via `suffix`, simpler config). The
generic backend's unique value is the non-OpenAI shapes; with it, hand-written
Claude/Gemini backends are no longer needed.
```

If the "Still deferred" sentence in the OpenAI blurb (or elsewhere) lists a "Claude backend" as a planned hand-written backend, update it to note the generic backend now covers Claude/Gemini via templates (leave SSE streaming and libsecret deferred).

- [ ] **Step 4: Commit**

```bash
git add examples/anthropic.json examples/gemini.json NOTES.md
git commit -m "docs(generic): example Anthropic/Gemini templates + NOTES"
```

---

## Final Review

After all tasks, dispatch a holistic reviewer over the whole branch diff, then use **superpowers:finishing-a-development-branch**.

Verify before finishing:
```bash
ninja -C build && meson test -C build
OPENAI_API_KEY=dummy MISTRAL_API_KEY=dummy ANTHROPIC_API_KEY=dummy meson test -C build
```
Expected: the full suite (now 12 test executables — added `text-util` and `generic-body`) passes in both runs.

---

## Self-Review (plan vs. spec)

- **Spec coverage:** transport generalization (T1), shared cleanup extraction (T2), `_build_body` structural substitution (T3), `_extract` dotted path (T4), the `LlmGhostGenericBackend` GObject + request flow + live-reload-safe owning refs (T5), factory `build_generic` + `"generic"` dispatch + GType test (T6), example templates + docs (T7). The in-process loopback header test is in T1; the warning-determinism convention is applied in the factory test (T6, keys-exported run).
- **Type/signature consistency:** `_llm_ghost_http_post_json_headers_async`, `_llm_ghost_clean_single_line`, `_llm_ghost_generic_build_body(template, prefix, suffix, model)`, `_llm_ghost_generic_extract(root, path, error)`, and `llm_ghost_generic_backend_new(url, headers, model, request_template, response_path)` are spelled identically everywhere they appear across tasks; the factory in T6 calls the exact T5 constructor signature; the backend in T5 calls the exact T3/T4 pure-function and T1/T2 helper signatures.
- **Ownership:** owning `json_object_ref` on headers/template (T5) guarantees lifetime across a settings reload and in-flight requests; `build_body` deep-copies before mutating (T3) so the stored template is never touched; the Bearer wrapper unrefs its one-entry headers object after the synchronous send setup (T1); the generic backend's `g_clear_pointer(..., json_object_unref)` mirrors finalize idioms.
- **No placeholders:** every code step has complete code; every run step states the command and expected outcome.
- **Decomposition:** the substitution walker mutates string nodes in place via `json_node_set_string` (uniform for object members and array elements), avoiding json-glib's missing array-element setter — noted in T3 so an implementer doesn't reach for a nonexistent API.
```
