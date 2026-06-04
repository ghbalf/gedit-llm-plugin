# Mistral Codestral Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `LlmGhostMistralBackend`, a `LlmGhostBackend` over Mistral's Codestral FIM endpoint (`POST {base}/fim/completions`, native `prompt`+`suffix`), built on the existing shared `llmghost-http-util`.

**Architecture:** Pure, unit-tested FIM body-builder + response extractor (prefer `choices[0].message.content`, fall back to `choices[0].text`) wrapped in a thin `GObject` that drives `llmghost-http-util`. Mirrors the existing OpenAI backend's file layout and idioms; no shared-helper extraction (the ~5-line `join_url` and ~15-line body-builder are kept local to stay decoupled).

**Tech Stack:** C (gnu11), GLib/GObject, libsoup-3.0, json-glib, GTK 3, meson/ninja, `g_test`.

---

## Background for the implementer

C/GObject library built with **meson**. Build: `meson compile -C build`. Test: `meson test -C build <name> -v`; full suite `meson test -C build`. Build dir `build/` exists.

**The developer cannot run the GUI (SSH, no display). The automated test suite is the sole correctness gate — every task ends green.**

This backend is a near-twin of the existing **OpenAI backend** — read these first and mirror their idioms exactly:
- `lib/llmghost-openai-backend.c` — the canonical structure: pure functions (body-builder, extractor) at the top behind an internal header, then the `GObject` (struct, `G_DEFINE_TYPE_WITH_CODE` + `G_IMPLEMENT_INTERFACE`, `join_url`, `on_http_done`, `*_request`/`*_request_finish`, iface init, finalize, class_init, init, `pick`/`pick_nullable`, constructor).
- `lib/llmghost-openai-backend.h` / `-internal.h` — installed public header vs non-installed testing header.
- `tests/test-openai-body.c` — the json-glib build/parse test idiom.
- `lib/llmghost-http-util.h` — `_llm_ghost_http_post_json_async(session, url, bearer, json_body /*owned*/, cancellable, callback, user_data)` and `_llm_ghost_http_post_json_finish(result, error) -> JsonNode*` (owned; `json_node_unref`).
- `lib/meson.build` (`llmghost_sources`, `llmghost_headers`), `tests/meson.build`, `lib/llmghost.h`, `tests/demo.c`.

**Note on the JsonBuilder leak-free idiom:** when serialising, `json_builder_get_root()` is transfer-full and `json_generator_set_root()` is transfer-none, so the root node must be unref'd. The body-builder below already does this — do not drop the `json_node_unref (root)`.

**Internal/util headers are NOT installed** — only the public `.h` goes in `llmghost_headers`.

---

## File structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-mistral-backend.h` | **New (installed).** `G_DECLARE_FINAL_TYPE` + constructor. |
| `lib/llmghost-mistral-backend-internal.h` | **New (not installed).** Pure body-builder + extractor. |
| `lib/llmghost-mistral-backend.c` | **New.** Pure functions (Task 1) + GObject (Task 2). |
| `tests/test-mistral-body.c` | **New.** Unit tests for the pure functions. |
| `lib/llmghost.h` | **Modify.** Include the new public header. |
| `lib/meson.build` | **Modify.** Add the `.c` to sources; public `.h` to installed headers. |
| `tests/meson.build` | **Modify.** Register `test-mistral-body` (unit). |
| `tests/demo.c` | **Modify.** Add `mistral` to the `LLMGHOST_BACKEND` selector. |
| `NOTES.md` | **Modify.** Mark the backend done + record the JSON-settings direction. |

---

## Task 1: Mistral pure functions + headers + unit tests

The pure functions are fully unit-tested. TDD: failing test first, then implement.

**Files:**
- Create: `tests/test-mistral-body.c`
- Create: `lib/llmghost-mistral-backend.h` (type macro + constructor decl — needed so the internal header and the eventual GObject share one declaration)
- Create: `lib/llmghost-mistral-backend-internal.h`
- Create: `lib/llmghost-mistral-backend.c` (pure functions only)
- Modify: `lib/meson.build`, `tests/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-mistral-body.c`:

```c
#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-mistral-backend-internal.h"

static JsonObject *
parse_object (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonObject *obj = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  return obj;
}

static JsonNode *
parse_node (const char *json)
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
test_fim_body (void)
{
  char *body = _llm_ghost_mistral_build_fim_body ("codestral-latest", "int main", "}", 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"),  ==, "codestral-latest");
  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==, "int main");
  g_assert_cmpstr (json_object_get_string_member (obj, "suffix"), ==, "}");
  g_assert_cmpint (json_object_get_int_member (obj, "max_tokens"), ==, 64);
  g_assert_true (ABS (json_object_get_double_member (obj, "temperature") - 0.2) < 1e-9);

  JsonArray *stop = json_object_get_array_member (obj, "stop");
  g_assert_cmpint (json_array_get_length (stop), ==, 1);
  g_assert_cmpstr (json_array_get_string_element (stop, 0), ==, "\n");

  json_object_unref (obj);
  g_free (body);
}

static void
test_extract_message_content (void)
{
  JsonNode *node = parse_node (
      "{\"choices\":[{\"message\":{\"content\":\"sum(a, b)\"}}]}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "sum(a, b)");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_text_fallback (void)
{
  /* No "message" → fall back to choices[0].text. */
  JsonNode *node = parse_node ("{\"choices\":[{\"text\":\"fallback\"}]}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "fallback");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_empty_choices (void)
{
  JsonNode *node = parse_node ("{\"choices\":[]}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_missing_choices (void)
{
  JsonNode *node = parse_node ("{}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_error_object (void)
{
  JsonNode *node = parse_node ("{\"error\":{\"message\":\"unauthorized\"}}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_null (out);
  g_assert_nonnull (error);
  g_assert_nonnull (g_strstr_len (error->message, -1, "unauthorized"));
  g_clear_error (&error);
  json_node_unref (node);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mistral-body/fim",              test_fim_body);
  g_test_add_func ("/mistral-body/extract-message",  test_extract_message_content);
  g_test_add_func ("/mistral-body/extract-text-fallback", test_extract_text_fallback);
  g_test_add_func ("/mistral-body/extract-empty",    test_extract_empty_choices);
  g_test_add_func ("/mistral-body/extract-missing",  test_extract_missing_choices);
  g_test_add_func ("/mistral-body/extract-error",    test_extract_error_object);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test and confirm it fails**

Append to `tests/meson.build`:

```meson
test_mistral_body = executable(
  'test-mistral-body',
  'test-mistral-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('mistral-body', test_mistral_body, suite: 'unit')
```

Run `meson compile -C build`. Expected red: missing header or undefined references to the pure functions.

- [ ] **Step 3: Create the public header**

Create `lib/llmghost-mistral-backend.h`:

```c
#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_MISTRAL_BACKEND (llm_ghost_mistral_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostMistralBackend, llm_ghost_mistral_backend,
                      LLM_GHOST, MISTRAL_BACKEND, GObject)

/**
 * llm_ghost_mistral_backend_new:
 * @base_url: API base. NULL/"" → $LLMGHOST_MISTRAL_BASE_URL or the Codestral default.
 * @model:    model id. NULL/"" → $LLMGHOST_MISTRAL_MODEL or "codestral-latest".
 * @api_key:  bearer token. NULL/"" → $LLMGHOST_MISTRAL_API_KEY or no auth.
 *
 * Talks to Mistral's Codestral FIM endpoint (POST {base}/fim/completions),
 * sending the prefix as `prompt` and the suffix as `suffix`.
 */
LlmGhostBackend *llm_ghost_mistral_backend_new (const char *base_url,
                                                const char *model,
                                                const char *api_key);

G_END_DECLS
```

- [ ] **Step 4: Create the internal header**

Create `lib/llmghost-mistral-backend-internal.h`:

```c
#pragma once

/* Testing-only internal API. NOT installed. Pure FIM request-body builder and
 * response extractor for direct unit testing. */

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

char *_llm_ghost_mistral_build_fim_body    (const char *model,
                                            const char *prefix,
                                            const char *suffix,
                                            guint       max_tokens,
                                            double      temperature);

/* Pull the completion from a parsed Codestral FIM response @root: prefer
 * choices[0].message.content, fall back to choices[0].text. Returns "" for
 * no/empty choices; NULL + @error when the body carries an API error object. */
char *_llm_ghost_mistral_extract_completion (JsonNode  *root,
                                            GError    **error);

G_END_DECLS
```

- [ ] **Step 5: Implement the pure functions**

Create `lib/llmghost-mistral-backend.c`:

```c
#include "llmghost-mistral-backend.h"
#include "llmghost-mistral-backend-internal.h"

#include <string.h>

/* ---- request body builder ----------------------------------------------- */

char *
_llm_ghost_mistral_build_fim_body (const char *model,
                                   const char *prefix,
                                   const char *suffix,
                                   guint       max_tokens,
                                   double      temperature)
{
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, model ? model : "");
  json_builder_set_member_name (b, "prompt");
  json_builder_add_string_value (b, prefix ? prefix : "");
  json_builder_set_member_name (b, "suffix");
  json_builder_add_string_value (b, suffix ? suffix : "");
  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, max_tokens);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, temperature);
  json_builder_set_member_name (b, "stop");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "\n");
  json_builder_end_array (b);

  json_builder_end_object (b);

  JsonGenerator *gen  = json_generator_new ();
  JsonNode      *root = json_builder_get_root (b);   /* transfer full */
  json_generator_set_root (gen, root);               /* transfer none */
  char *out = json_generator_to_data (gen, NULL);
  json_node_unref (root);
  g_object_unref (gen);
  g_object_unref (b);
  return out;
}

/* ---- response extraction ------------------------------------------------ */

char *
_llm_ghost_mistral_extract_completion (JsonNode *root, GError **error)
{
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "mistral: malformed response");
      return NULL;
    }

  JsonObject *obj = json_node_get_object (root);

  if (json_object_has_member (obj, "error"))
    {
      JsonNode   *en  = json_object_get_member (obj, "error");
      const char *msg = NULL;
      if (JSON_NODE_HOLDS_OBJECT (en))
        {
          JsonObject *eo = json_node_get_object (en);
          if (json_object_has_member (eo, "message"))
            msg = json_object_get_string_member (eo, "message");
        }
      else if (JSON_NODE_HOLDS_VALUE (en))
        {
          msg = json_node_get_string (en);
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "mistral: %s", msg ? msg : "(error)");
      return NULL;
    }

  if (!json_object_has_member (obj, "choices"))
    return g_strdup ("");

  JsonArray *choices = json_object_get_array_member (obj, "choices");
  if (choices == NULL || json_array_get_length (choices) == 0)
    return g_strdup ("");

  JsonObject *choice = json_array_get_object_element (choices, 0);

  /* Codestral FIM returns a chat-style choice; tolerate a plain-text shape. */
  if (json_object_has_member (choice, "message"))
    {
      JsonObject *m = json_object_get_object_member (choice, "message");
      if (m != NULL && json_object_has_member (m, "content"))
        {
          const char *content = json_object_get_string_member (m, "content");
          return g_strdup (content ? content : "");
        }
    }

  if (json_object_has_member (choice, "text"))
    {
      const char *text = json_object_get_string_member (choice, "text");
      return g_strdup (text ? text : "");
    }

  return g_strdup ("");
}
```

- [ ] **Step 6: Add the source + public header to the library**

In `lib/meson.build`: add `'llmghost-mistral-backend.c',` to `llmghost_sources`, and `'llmghost-mistral-backend.h',` to `llmghost_headers` (installed; the `-internal.h` is not).

- [ ] **Step 7: Build and run**

Run `meson compile -C build && meson test -C build mistral-body -v`. Expected: builds clean; `mistral-body` OK, 6 subtests pass.

- [ ] **Step 8: Confirm the whole unit suite passes**

Run `meson test -C build --suite unit -v`. Expected: all unit tests pass (fim-tokens, ollama-body, fake-backend, mock-backend, ghost-accept, http-util, openai-body, mistral-body).

- [ ] **Step 9: Commit**

```bash
git add lib/llmghost-mistral-backend.h lib/llmghost-mistral-backend-internal.h \
        lib/llmghost-mistral-backend.c tests/test-mistral-body.c lib/meson.build \
        tests/meson.build
git commit -m "feat: Mistral Codestral FIM body builder + extractor (pure)

Pure FIM request-body builder and response extractor (message.content
with a text fallback), unit-tested. GObject wrapper follows.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Mistral backend GObject + iface + constructor

Wrap the pure functions in a `LlmGhostBackend` driving `llmghost-http-util`.

**Files:**
- Modify: `lib/llmghost-mistral-backend.c` (append GObject machinery + includes)
- Modify: `lib/llmghost.h`

- [ ] **Step 1: Add the libsoup + util includes**

In `lib/llmghost-mistral-backend.c`, add after the existing includes (after `#include <string.h>`):

```c
#include <libsoup/soup.h>
#include "llmghost-http-util.h"
```

- [ ] **Step 2: Append the GObject machinery**

Append the following to the END of `lib/llmghost-mistral-backend.c`:

```c
/* ---- type --------------------------------------------------------------- */

#define DEFAULT_BASE_URL     "https://codestral.mistral.ai/v1"
#define DEFAULT_MODEL        "codestral-latest"
#define DEFAULT_MAX_TOKENS   64
#define DEFAULT_TEMPERATURE  0.2
#define REQUEST_TIMEOUT_SEC  30

struct _LlmGhostMistralBackend
{
  GObject       parent_instance;

  SoupSession  *session;
  char         *base_url;
  char         *model;
  char         *api_key;     /* NULL = no auth */
  guint         max_tokens;
  double        temperature;
};

static void llm_ghost_mistral_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostMistralBackend, llm_ghost_mistral_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_mistral_backend_iface_init))

/* ---- request flow ------------------------------------------------------- */

static char *
join_url (const char *base, const char *endpoint)
{
  gsize n = strlen (base);
  if (n > 0 && base[n - 1] == '/')
    return g_strconcat (base, endpoint, NULL);
  return g_strconcat (base, "/", endpoint, NULL);
}

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

  char *out = _llm_ghost_mistral_extract_completion (root, &error);
  json_node_unref (root);

  if (out == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  g_task_return_pointer (task, out, g_free);
  g_object_unref (task);
}

static void
mistral_request (LlmGhostBackend     *backend,
                 const char          *prefix,
                 const char          *suffix,
                 GCancellable        *cancellable,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
  LlmGhostMistralBackend *self = LLM_GHOST_MISTRAL_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  char *url  = join_url (self->base_url, "fim/completions");
  char *body = _llm_ghost_mistral_build_fim_body (self->model, prefix, suffix,
                                                  self->max_tokens, self->temperature);

  _llm_ghost_http_post_json_async (self->session, url, self->api_key, body,
                                   cancellable, on_http_done, task);
  g_free (url);
}

static char *
mistral_request_finish (LlmGhostBackend *backend, GAsyncResult *result, GError **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_mistral_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = mistral_request;
  iface->request_finish = mistral_request_finish;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llm_ghost_mistral_backend_finalize (GObject *object)
{
  LlmGhostMistralBackend *self = LLM_GHOST_MISTRAL_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->model,    g_free);
  g_clear_pointer (&self->api_key,  g_free);
  G_OBJECT_CLASS (llm_ghost_mistral_backend_parent_class)->finalize (object);
}

static void
llm_ghost_mistral_backend_class_init (LlmGhostMistralBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_mistral_backend_finalize;
}

static void
llm_ghost_mistral_backend_init (LlmGhostMistralBackend *self)
{
  self->session     = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
  self->max_tokens  = DEFAULT_MAX_TOKENS;
  self->temperature = DEFAULT_TEMPERATURE;
}

/* ---- construction ------------------------------------------------------- */

static char *
pick (const char *arg, const char *env_name, const char *fallback)
{
  if (arg != NULL && *arg != '\0')
    return g_strdup (arg);
  const char *e = g_getenv (env_name);
  if (e != NULL && *e != '\0')
    return g_strdup (e);
  return g_strdup (fallback);
}

static char *
pick_nullable (const char *arg, const char *env_name)
{
  if (arg != NULL && *arg != '\0')
    return g_strdup (arg);
  const char *e = g_getenv (env_name);
  if (e != NULL && *e != '\0')
    return g_strdup (e);
  return NULL;
}

LlmGhostBackend *
llm_ghost_mistral_backend_new (const char *base_url,
                               const char *model,
                               const char *api_key)
{
  LlmGhostMistralBackend *self = g_object_new (LLM_GHOST_TYPE_MISTRAL_BACKEND, NULL);

  self->base_url = pick (base_url, "LLMGHOST_MISTRAL_BASE_URL", DEFAULT_BASE_URL);
  self->model    = pick (model,    "LLMGHOST_MISTRAL_MODEL",    DEFAULT_MODEL);
  self->api_key  = pick_nullable (api_key, "LLMGHOST_MISTRAL_API_KEY");

  return LLM_GHOST_BACKEND (self);
}
```

- [ ] **Step 3: Add to the umbrella header**

In `lib/llmghost.h`, add after the openai include:

```c
#include "llmghost-mistral-backend.h"
```

- [ ] **Step 4: Build and verify everything still passes**

Run `meson compile -C build && meson test -C build`. Expected: builds clean; ALL suites green — `unit` (including `mistral-body` 6, `openai-body` 9, `http-util` 4) and `controller` (gui, 9).

- [ ] **Step 5: Commit**

```bash
git add lib/llmghost-mistral-backend.c lib/llmghost.h
git commit -m "feat: LlmGhostMistralBackend GObject over llmghost-http-util

Implements LlmGhostBackend against the Codestral FIM endpoint with
optional Bearer auth and constructor + LLMGHOST_MISTRAL_* env config.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Demo backend selector

Make the Mistral backend reachable via `LLMGHOST_BACKEND=mistral`.

**Files:**
- Modify: `tests/demo.c`

- [ ] **Step 1: Add the mistral branch**

In `tests/demo.c`, the selector currently looks like:

```c
  if (which != NULL && g_ascii_strcasecmp (which, "openai") == 0)
    {
      /* base/model/key/mode all read from LLMGHOST_OPENAI_* by the ctor. */
      backend = llm_ghost_openai_backend_new (NULL, NULL, NULL,
                                              LLM_GHOST_OPENAI_MODE_CHAT);
      gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (OpenAI)");
    }
  else
    {
```

Insert a `mistral` branch between the OpenAI `if` block and the `else`, so it reads:

```c
  if (which != NULL && g_ascii_strcasecmp (which, "openai") == 0)
    {
      /* base/model/key/mode all read from LLMGHOST_OPENAI_* by the ctor. */
      backend = llm_ghost_openai_backend_new (NULL, NULL, NULL,
                                              LLM_GHOST_OPENAI_MODE_CHAT);
      gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (OpenAI)");
    }
  else if (which != NULL && g_ascii_strcasecmp (which, "mistral") == 0)
    {
      /* base/model/key all read from LLMGHOST_MISTRAL_* by the ctor. */
      backend = llm_ghost_mistral_backend_new (NULL, NULL, NULL);
      gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (Mistral Codestral)");
    }
  else
    {
```

(Leave the `else { ... ollama ... }` block unchanged.)

- [ ] **Step 2: Update the demo's header comment**

In the top doc-comment of `tests/demo.c`, update the `LLMGHOST_BACKEND` line and add a Mistral config line:

```c
 *   LLMGHOST_BACKEND        (default ollama; "openai" or "mistral" for those backends)
 *   LLMGHOST_OPENAI_BASE_URL / _MODEL / _API_KEY / _MODE  (OpenAI backend config)
 *   LLMGHOST_MISTRAL_BASE_URL / _MODEL / _API_KEY         (Mistral backend config)
```

(The existing `LLMGHOST_OPENAI_*` line may already be present from the OpenAI work; if so, just add the `LLMGHOST_MISTRAL_*` line and update the `LLMGHOST_BACKEND` line.)

- [ ] **Step 3: Build and confirm the suite is still green**

Run `meson compile -C build && meson test -C build`. Expected: demo builds (links `llm_ghost_mistral_backend_new`); all tests green.

- [ ] **Step 4: Commit**

```bash
git add tests/demo.c
git commit -m "feat: demo LLMGHOST_BACKEND=mistral selector

Run the demo against the Codestral backend with LLMGHOST_BACKEND=mistral;
config via LLMGHOST_MISTRAL_* env vars.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Documentation + full verification

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Mark the Mistral backend done**

In `NOTES.md`, under the `## Phase 6 — cloud LLM provider backends` heading, immediately after the existing "**OpenAI-compatible backend landed ...**" paragraph, add:

```markdown
**Mistral Codestral backend landed 2026-06-04.** `LlmGhostMistralBackend`
hits the Codestral FIM endpoint (`POST {base}/fim/completions`, native
`prompt`+`suffix`), response = `choices[0].message.content` (with a `text`
fallback). Config via constructor + `LLMGHOST_MISTRAL_{BASE_URL,MODEL,API_KEY}`,
optional Bearer auth; default base `https://codestral.mistral.ai/v1`, model
`codestral-latest`. Built on `llmghost-http-util`; reachable in the demo via
`LLMGHOST_BACKEND=mistral`. Covered by the `mistral-body` unit suite.
```

- [ ] **Step 2: Record the JSON-settings direction**

In `NOTES.md`, find the `### Architectural prerequisites` subsection under Phase 6. Replace this exact current block (item 1):

```markdown
1. **Settings UI**: env-var-only stops scaling once the user has
   multiple backends configured. GSettings schema + a small Prefs
   widget (libpeas-gtk knows how to surface it). Settings include:
   active backend, per-backend params (host/model/token-set for Ollama;
   model name + base URL for OpenAI; etc.), debounce/timeout overrides.
```

with this text (keep the numbered-list position):

```markdown
1. **Settings**: env-var-only stops scaling once the user has multiple
   backends configured. Decision (2026-06-04): use a **human-editable JSON
   config file** (e.g. `~/.config/llmghost/settings.json`, XDG-based, parsed
   with json-glib, watched via `GFileMonitor` for live reload), **not
   GSettings/dconf** — this is a gedit plugin, so users edit the config in
   gedit itself, with no schema-compile or `dconf-editor` step. Settings
   include: active backend, per-backend params, debounce/timeout overrides.
   The backend constructors (`*_backend_new(base, model, key, ...)`) are what
   the loader will call, so the backends are already forward-compatible. A
   small Prefs entry point can later just open the JSON file in the editor.
```

- [ ] **Step 3: Commit**

```bash
git add NOTES.md
git commit -m "docs: note Mistral backend + JSON-settings direction in NOTES.md

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 4: Full verification**

Run `meson test -C build`. Expected: all targets green — unit suite now `fim-tokens`, `ollama-body`, `fake-backend`, `mock-backend`, `ghost-accept`, `http-util` (4), `openai-body` (9), `mistral-body` (6); gui `controller` (9).

Run `meson compile -C build`. Expected: clean.

---

## Self-review

**Spec coverage:**
- FIM body (`model`/`prompt`/`suffix`/`max_tokens`/`temperature`/`stop:["\n"]`) → Task 1 `_build_fim_body` + test. ✓
- Response extraction (message.content → text fallback, empty/missing choices, error object) → Task 1 `_extract_completion` + 5 tests. ✓
- GObject + iface + constructor + env config + optional Bearer + URL join → Task 2. ✓
- Defaults (Codestral base, codestral-latest, NULL key, 64/0.2) → Task 2 `pick`/`init`/`new`. ✓
- Demo reachability via `LLMGHOST_BACKEND=mistral` → Task 3. ✓
- Installed vs non-installed headers (public `.h` installed; `-internal.h` not) → Task 1 meson step. ✓
- NOTES: backend landed + JSON-settings direction recorded → Task 4. ✓

**Placeholder scan:** No TBD/TODO; every code step is complete. ✓

**Type/name consistency:** `_llm_ghost_mistral_build_fim_body(model, prefix, suffix, max_tokens, temperature)` and `_llm_ghost_mistral_extract_completion(root, error)` identical across internal header (T1), impl (T1), tests (T1), and the GObject caller (T2); `llm_ghost_mistral_backend_new(base_url, model, api_key)` identical in public header (T1), impl (T2), and demo (T3); `LLM_GHOST_TYPE_MISTRAL_BACKEND` / `LLM_GHOST_MISTRAL_BACKEND` macro names consistent. `_llm_ghost_http_post_json_async` ownership of `body` honored (not freed in `mistral_request`). The JsonBuilder root-node is unref'd (leak-free idiom). ✓
```
1. Pure FIM builder + extractor + unit tests → verify: meson test --suite unit green
2. GObject + ctor                            → verify: meson test green (all)
3. Demo selector                             → verify: meson compile + meson test green
4. Docs + full run                           → verify: meson test green end-to-end
```
