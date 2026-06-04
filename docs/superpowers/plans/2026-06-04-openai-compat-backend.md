# OpenAI-Compatible Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `LlmGhostOpenAIBackend` (an OpenAI-compatible HTTP backend with native-FIM `/v1/completions` and prompt-FIM `/v1/chat/completions` modes), built on a new shared `llmghost-http-util` extracted from the Ollama backend.

**Architecture:** Extract the libsoup POST + JSON-parse plumbing into `llmghost-http-util` (async post → parsed `JsonNode`), refactor the Ollama backend onto it, then implement the OpenAI backend as pure testable functions (two body-builders + chat-cleanup + response-extractor) wrapped in a thin `GObject` that drives the util.

**Tech Stack:** C (gnu11), GLib/GObject, libsoup-3.0, json-glib, GTK 3, meson/ninja, `g_test`.

---

## Background for the implementer

C/GObject library built with **meson**. Build: `meson compile -C build`. Test: `meson test -C build <name> -v`. The full suite is `meson test -C build`. All tests in this plan are **display-free** and go in the `unit` suite (no Xvfb).

**The developer cannot run the GUI manually (SSH, no display). The automated test suite is the sole correctness gate — every task ends green or it isn't done.**

Patterns to follow (Read them before editing):
- `lib/llmghost-ollama-backend.c` — the existing HTTP backend. Note the pure body-builder `_llm_ghost_ollama_build_request_body` (exposed via `lib/llmghost-ollama-backend-internal.h`, tested in `tests/test-ollama-body.c`) separated from the async libsoup handler `on_libsoup_response` + `ollama_request`.
- `lib/llmghost-backend.h` — the `LlmGhostBackend` GInterface: `request(prefix, suffix, cancellable, callback, user_data)` and `request_finish(result, error) → char*`.
- `tests/test-ollama-body.c` — the json-glib round-trip test idiom (`parse_object`, `json_object_get_*_member`).
- `lib/meson.build` — `llmghost_sources`, `llmghost_headers` (installed; do NOT add internal/util headers here), `llmghost_dep`.
- `tests/meson.build` — `test(...)` registration with `suite: 'unit'`.

**Internal/util headers are NOT installed** — only add public headers to `llmghost_headers`.

---

## File structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-http-util.h` | **New (not installed).** Async `_llm_ghost_http_post_json_{async,finish}` declarations. |
| `lib/llmghost-http-util.c` | **New.** libsoup POST + status-check + JSON-parse, returns a parsed `JsonNode`. |
| `tests/test-http-util.c` | **New.** In-process `SoupServer` loopback tests for the util. |
| `lib/llmghost-ollama-backend.c` | **Modify.** Route `request()` through the util; drop the duplicated `on_libsoup_response`. |
| `lib/llmghost-openai-backend.h` | **New (installed).** Mode enum, `G_DECLARE_FINAL_TYPE`, constructor. |
| `lib/llmghost-openai-backend-internal.h` | **New (not installed).** Pure body-builders + cleanup + extractor. |
| `lib/llmghost-openai-backend.c` | **New.** Pure functions (Task 3) + `GObject`/iface/constructor (Task 4). |
| `tests/test-openai-body.c` | **New.** Unit tests for the pure functions. |
| `lib/llmghost.h` | **Modify.** Include the new public header. |
| `lib/meson.build` | **Modify.** Add the two new `.c` to sources; add the public header to installed headers. |
| `tests/meson.build` | **Modify.** Register `test-http-util` and `test-openai-body` (unit). |
| `tests/demo.c` | **Modify.** `LLMGHOST_BACKEND` selector. |
| `NOTES.md` | **Modify.** Mark Phase 6 OpenAI backend done. |

---

## Task 1: `llmghost-http-util` + in-process SoupServer test

Build the shared async helper first, proven by a loopback server test. Red→green: the test references functions that don't exist (link error), then we add them.

**Files:**
- Create: `tests/test-http-util.c`
- Create: `lib/llmghost-http-util.h`
- Create: `lib/llmghost-http-util.c`
- Modify: `lib/meson.build`, `tests/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-http-util.c`:

```c
#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "llmghost-http-util.h"

/* ---- in-process test server -------------------------------------------- */

typedef struct {
  char *last_auth;          /* captured Authorization header ("" if none) */
  char *last_content_type;  /* captured Content-Type */
  char *last_body;          /* captured request body */
} Captured;

static void
server_cb (SoupServer *server, SoupServerMessage *m, const char *path,
           GHashTable *query, gpointer user_data)
{
  (void) server; (void) query;
  Captured *cap = user_data;

  SoupMessageHeaders *h = soup_server_message_get_request_headers (m);
  const char *auth = soup_message_headers_get_one (h, "Authorization");
  const char *ct   = soup_message_headers_get_one (h, "Content-Type");
  g_clear_pointer (&cap->last_auth, g_free);
  g_clear_pointer (&cap->last_content_type, g_free);
  g_clear_pointer (&cap->last_body, g_free);
  cap->last_auth = g_strdup (auth ? auth : "");
  cap->last_content_type = g_strdup (ct ? ct : "");

  SoupMessageBody *body = soup_server_message_get_request_body (m);
  cap->last_body = g_strndup (body->data ? body->data : "", body->length);

  if (g_strcmp0 (path, "/ok") == 0)
    {
      const char *resp = "{\"hello\":\"world\"}";
      soup_server_message_set_status (m, 200, NULL);
      soup_server_message_set_response (m, "application/json",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
  else if (g_strcmp0 (path, "/bad") == 0)
    {
      const char *resp = "kaboom";
      soup_server_message_set_status (m, 500, NULL);
      soup_server_message_set_response (m, "text/plain",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
  else /* /malformed */
    {
      const char *resp = "not json {";
      soup_server_message_set_status (m, 200, NULL);
      soup_server_message_set_response (m, "application/json",
                                        SOUP_MEMORY_COPY, resp, strlen (resp));
    }
}

typedef struct {
  SoupServer *server;
  char       *base;       /* e.g. "http://127.0.0.1:PORT/" */
  Captured    cap;
} Srv;

static Srv *
srv_new (void)
{
  Srv *s = g_new0 (Srv, 1);
  s->server = soup_server_new (NULL, NULL);
  soup_server_add_handler (s->server, NULL, server_cb, &s->cap, NULL);

  GError *error = NULL;
  g_assert_true (soup_server_listen_local (s->server, 0,
                                           SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error (error);

  GSList *uris = soup_server_get_uris (s->server);
  g_assert_nonnull (uris);
  s->base = g_uri_to_string (uris->data);   /* trailing slash included */
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);
  return s;
}

static void
srv_free (Srv *s)
{
  g_clear_object (&s->server);
  g_free (s->base);
  g_free (s->cap.last_auth);
  g_free (s->cap.last_content_type);
  g_free (s->cap.last_body);
  g_free (s);
}

/* ---- async driver ------------------------------------------------------ */

typedef struct {
  GMainLoop *loop;
  JsonNode  *node;
  GError    *error;
} Wait;

static void
on_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void) source;
  Wait *w = user_data;
  w->node = _llm_ghost_http_post_json_finish (result, &w->error);
  g_main_loop_quit (w->loop);
}

static JsonNode *
post (Srv *s, const char *path, const char *bearer, const char *body, GError **error)
{
  SoupSession *session = soup_session_new ();
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  Wait w = { .loop = loop };
  char *url = g_strconcat (s->base, path + 1, NULL);  /* base ends in '/', path starts '/' */

  _llm_ghost_http_post_json_async (session, url, bearer, g_strdup (body),
                                   NULL, on_done, &w);
  g_main_loop_run (loop);

  g_free (url);
  g_main_loop_unref (loop);
  g_object_unref (session);
  if (error != NULL)
    *error = w.error;
  else
    g_clear_error (&w.error);
  return w.node;
}

/* ---- tests ------------------------------------------------------------- */

static void
test_ok_with_bearer (void)
{
  Srv *s = srv_new ();
  GError *error = NULL;
  JsonNode *node = post (s, "/ok", "secret", "{\"x\":1}", &error);

  g_assert_no_error (error);
  g_assert_nonnull (node);
  JsonObject *obj = json_node_get_object (node);
  g_assert_cmpstr (json_object_get_string_member (obj, "hello"), ==, "world");

  g_assert_cmpstr (s->cap.last_auth, ==, "Bearer secret");
  g_assert_true (g_str_has_prefix (s->cap.last_content_type, "application/json"));
  g_assert_cmpstr (s->cap.last_body, ==, "{\"x\":1}");

  json_node_unref (node);
  srv_free (s);
}

static void
test_ok_without_bearer (void)
{
  Srv *s = srv_new ();
  JsonNode *node = post (s, "/ok", NULL, "{}", NULL);
  g_assert_nonnull (node);
  g_assert_cmpstr (s->cap.last_auth, ==, "");   /* no Authorization header */
  json_node_unref (node);
  srv_free (s);
}

static void
test_http_500_is_error (void)
{
  Srv *s = srv_new ();
  GError *error = NULL;
  JsonNode *node = post (s, "/bad", NULL, "{}", &error);
  g_assert_null (node);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (error->message, -1, "500"));    /* status in message */
  g_assert_nonnull (g_strstr_len (error->message, -1, "kaboom")); /* snippet in message */
  g_clear_error (&error);
  srv_free (s);
}

static void
test_malformed_json_is_error (void)
{
  Srv *s = srv_new ();
  GError *error = NULL;
  JsonNode *node = post (s, "/malformed", NULL, "{}", &error);
  g_assert_null (node);
  g_assert_nonnull (error);
  g_clear_error (&error);
  srv_free (s);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/http-util/ok-with-bearer",    test_ok_with_bearer);
  g_test_add_func ("/http-util/ok-without-bearer", test_ok_without_bearer);
  g_test_add_func ("/http-util/http-500",          test_http_500_is_error);
  g_test_add_func ("/http-util/malformed-json",    test_malformed_json_is_error);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test and confirm it fails**

Append to `tests/meson.build`:

```meson
test_http_util = executable(
  'test-http-util',
  'test-http-util.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('http-util', test_http_util, suite: 'unit')
```

Run: `meson compile -C build`
Expected: failure — `llmghost-http-util.h: No such file or directory`, or `undefined reference to _llm_ghost_http_post_json_async`. Either is the acceptable red state.

- [ ] **Step 3: Create the util header**

Create `lib/llmghost-http-util.h`:

```c
#pragma once

/* Internal (NOT installed) shared HTTP helper for the HTTP-based backends.
 * POSTs a JSON body and hands back the parsed JSON response. */

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* POST @json_body to @url. Attaches "Authorization: Bearer <bearer>" iff
 * @bearer is non-NULL and non-empty. Takes ownership of @json_body (frees it).
 * @session and @cancellable are borrowed. */
void       _llm_ghost_http_post_json_async  (SoupSession         *session,
                                             const char          *url,
                                             const char          *bearer,
                                             char                *json_body,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);

/* Returns the parsed root JsonNode (caller owns via json_node_unref), or
 * NULL + @error on transport failure, non-2xx HTTP (message carries the
 * status code and a body snippet), or malformed JSON. */
JsonNode * _llm_ghost_http_post_json_finish (GAsyncResult        *result,
                                             GError             **error);

G_END_DECLS
```

- [ ] **Step 4: Implement the util**

Create `lib/llmghost-http-util.c`:

```c
#include "llmghost-http-util.h"
#include <string.h>

static void
on_soup_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask       *task    = G_TASK (user_data);
  SoupSession *session = SOUP_SESSION (source);
  SoupMessage *msg     = SOUP_MESSAGE (g_task_get_task_data (task));
  GError      *error   = NULL;

  GBytes *body = soup_session_send_and_read_finish (session, result, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  guint       status = soup_message_get_status (msg);
  gsize       len    = 0;
  const char *data   = body ? g_bytes_get_data (body, &len) : NULL;

  if (status < 200 || status >= 300)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "HTTP %u: %.*s", status,
                               (int) MIN (len, 256u), data ? data : "");
      g_clear_pointer (&body, g_bytes_unref);
      g_object_unref (task);
      return;
    }

  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, data ? data : "", (gssize) len, &error))
    {
      g_task_return_error (task, error);
      g_object_unref (parser);
      g_clear_pointer (&body, g_bytes_unref);
      g_object_unref (task);
      return;
    }

  JsonNode *root = json_parser_get_root (parser);
  JsonNode *copy = root ? json_node_copy (root) : NULL;
  g_object_unref (parser);
  g_clear_pointer (&body, g_bytes_unref);

  if (copy == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "empty JSON response");
      g_object_unref (task);
      return;
    }

  g_task_return_pointer (task, copy, (GDestroyNotify) json_node_unref);
  g_object_unref (task);
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

  if (bearer != NULL && *bearer != '\0')
    {
      SoupMessageHeaders *h = soup_message_get_request_headers (msg);
      char *auth = g_strdup_printf ("Bearer %s", bearer);
      soup_message_headers_append (h, "Authorization", auth);
      g_free (auth);
    }

  GBytes *bytes = g_bytes_new_take (json_body, strlen (json_body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  g_bytes_unref (bytes);

  /* Keep the SoupMessage alive until the handler reads its status. */
  g_task_set_task_data (task, msg, g_object_unref);

  soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT,
                                    cancellable, on_soup_response, task);
}

JsonNode *
_llm_ghost_http_post_json_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}
```

- [ ] **Step 5: Add the util to the library sources**

In `lib/meson.build`, add to `llmghost_sources` (do NOT add the header to `llmghost_headers` — it is not installed):

```meson
  'llmghost-http-util.c',
```

- [ ] **Step 6: Build and run**

Run: `meson compile -C build && meson test -C build http-util -v`
Expected: builds clean; `http-util` reports `OK`, 4 subtests pass.

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-http-util.h lib/llmghost-http-util.c \
        tests/test-http-util.c lib/meson.build tests/meson.build
git commit -m "feat: add llmghost-http-util shared POST/JSON helper

Async POST-JSON helper returning a parsed JsonNode, with an in-process
SoupServer loopback test (status, auth header, parse-error paths).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Refactor the Ollama backend onto the util

Behavior-preserving. Safety net: `test-ollama-body` (builder unchanged) and the `controller` gui test.

**Files:**
- Modify: `lib/llmghost-ollama-backend.c`

- [ ] **Step 1: Read the current async path**

Open `lib/llmghost-ollama-backend.c`. The `on_libsoup_response` handler (≈lines 103–181) and `ollama_request` (≈185–225) are what we replace; `_llm_ghost_ollama_build_request_body` stays untouched.

- [ ] **Step 2: Replace the include, handler, and request fn**

At the top, add the util include after the existing includes:

```c
#include "llmghost-http-util.h"
```

Delete the entire `on_libsoup_response` function. Replace `ollama_request` with the version below and add the new `on_ollama_response` handler just above it:

```c
/* ---- async response handler --------------------------------------------- */

static void
on_ollama_response (GObject *source, GAsyncResult *result, gpointer user_data)
{
  GTask    *task  = G_TASK (user_data);
  GError   *error = NULL;
  JsonNode *root  = _llm_ghost_http_post_json_finish (result, &error);

  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  JsonObject *obj = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  if (obj == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ollama: malformed JSON response");
      json_node_unref (root);
      g_object_unref (task);
      return;
    }

  if (json_object_has_member (obj, "error"))
    {
      const char *err = json_object_get_string_member (obj, "error");
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "ollama: %s", err ? err : "(no message)");
      json_node_unref (root);
      g_object_unref (task);
      return;
    }

  const char *response = json_object_has_member (obj, "response")
                             ? json_object_get_string_member (obj, "response")
                             : "";
  char *out = g_strdup (response ? response : "");
  json_node_unref (root);

  g_task_return_pointer (task, out, g_free);
  g_object_unref (task);
}

/* ---- LlmGhostBackend interface ------------------------------------------ */

static void
ollama_request (LlmGhostBackend     *backend,
                const char          *prefix,
                const char          *suffix,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  LlmGhostOllamaBackend *self = LLM_GHOST_OLLAMA_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  char *url = g_strdup_printf ("http://%s:%u/api/generate",
                               self->host, (unsigned) self->port);
  char *body = _llm_ghost_ollama_build_request_body (self->model, self->fim_tokens,
                                                     prefix, suffix,
                                                     self->num_predict, self->temperature);

  _llm_ghost_http_post_json_async (self->session, url, NULL, body,
                                   cancellable, on_ollama_response, task);
  g_free (url);
}
```

Note: `_llm_ghost_http_post_json_async` takes ownership of `body`, so we do not free it. `<string.h>` is no longer needed by this file only if nothing else uses it — leave the existing includes as they are (the builder still uses `g_strconcat`, not `strlen`); do not remove includes unless the compiler warns.

- [ ] **Step 3: Build and verify behavior preserved**

Run: `meson compile -C build && meson test -C build ollama-body controller -v`
Expected: builds clean; `ollama-body` (3 subtests) and `controller` (9 subtests) all pass.

- [ ] **Step 4: Commit**

```bash
git add lib/llmghost-ollama-backend.c
git commit -m "refactor: route Ollama backend through llmghost-http-util

Drop the duplicated libsoup/JSON plumbing; the backend now builds its
body + URL and delegates transport to the shared helper. Behaviour-
preserving (ollama-body + controller tests unchanged).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: OpenAI pure functions + headers + unit tests

The body-builders, chat-cleanup, and response-extractor are pure and fully unit-tested. The `GObject` wrapper comes in Task 4.

**Files:**
- Create: `tests/test-openai-body.c`
- Create: `lib/llmghost-openai-backend.h` (enum only for now)
- Create: `lib/llmghost-openai-backend-internal.h`
- Create: `lib/llmghost-openai-backend.c` (pure functions only)
- Modify: `lib/meson.build`, `tests/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-openai-body.c`:

```c
#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-openai-backend-internal.h"

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
test_completions_body (void)
{
  char *body = _llm_ghost_openai_build_completions_body ("m", "int main", "}", 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"),  ==, "m");
  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==, "int main");
  g_assert_cmpstr (json_object_get_string_member (obj, "suffix"), ==, "}");
  g_assert_cmpint (json_object_get_int_member (obj, "max_tokens"), ==, 64);
  g_assert_false (json_object_get_boolean_member (obj, "stream"));

  JsonArray *stop = json_object_get_array_member (obj, "stop");
  g_assert_cmpint (json_array_get_length (stop), ==, 1);
  g_assert_cmpstr (json_array_get_string_element (stop, 0), ==, "\n");

  json_object_unref (obj);
  g_free (body);
}

static void
test_chat_body (void)
{
  char *body = _llm_ghost_openai_build_chat_body ("m", "foo(", ")", 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"), ==, "m");
  JsonArray *msgs = json_object_get_array_member (obj, "messages");
  g_assert_cmpint (json_array_get_length (msgs), ==, 2);

  JsonObject *sys = json_array_get_object_element (msgs, 0);
  g_assert_cmpstr (json_object_get_string_member (sys, "role"), ==, "system");

  JsonObject *usr = json_array_get_object_element (msgs, 1);
  g_assert_cmpstr (json_object_get_string_member (usr, "role"), ==, "user");
  g_assert_cmpstr (json_object_get_string_member (usr, "content"), ==,
                   "<PREFIX>foo(</PREFIX>\n<SUFFIX>)</SUFFIX>");

  json_object_unref (obj);
  g_free (body);
}

static void
check_clean (const char *raw, const char *expect)
{
  char *got = _llm_ghost_openai_clean_chat_completion (raw);
  g_assert_cmpstr (got, ==, expect);
  g_free (got);
}

static void
test_clean_chat_completion (void)
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

static void
test_extract_completions (void)
{
  JsonNode *node = parse_node ("{\"choices\":[{\"text\":\"hello\"}]}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_COMPLETIONS, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "hello");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_chat_cleans (void)
{
  JsonNode *node = parse_node (
      "{\"choices\":[{\"message\":{\"content\":\"```\\nfoo()\\n```\"}}]}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_CHAT, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "foo()");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_error_object (void)
{
  JsonNode *node = parse_node ("{\"error\":{\"message\":\"bad key\"}}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_CHAT, &error);
  g_assert_null (out);
  g_assert_nonnull (error);
  g_assert_nonnull (g_strstr_len (error->message, -1, "bad key"));
  g_clear_error (&error);
  json_node_unref (node);
}

static void
test_extract_empty_choices (void)
{
  JsonNode *node = parse_node ("{\"choices\":[]}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_COMPLETIONS, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");   /* no suggestion */
  g_free (out);
  json_node_unref (node);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/openai-body/completions",      test_completions_body);
  g_test_add_func ("/openai-body/chat",             test_chat_body);
  g_test_add_func ("/openai-body/clean",            test_clean_chat_completion);
  g_test_add_func ("/openai-body/extract-completions", test_extract_completions);
  g_test_add_func ("/openai-body/extract-chat",     test_extract_chat_cleans);
  g_test_add_func ("/openai-body/extract-error",    test_extract_error_object);
  g_test_add_func ("/openai-body/extract-empty",    test_extract_empty_choices);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test and confirm it fails**

Append to `tests/meson.build`:

```meson
test_openai_body = executable(
  'test-openai-body',
  'test-openai-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('openai-body', test_openai_body, suite: 'unit')
```

Run: `meson compile -C build`
Expected: failure — `llmghost-openai-backend-internal.h: No such file or directory` or undefined references to the pure functions. Acceptable red.

- [ ] **Step 3: Create the public header (enum only)**

Create `lib/llmghost-openai-backend.h`:

```c
#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

typedef enum
{
  LLM_GHOST_OPENAI_MODE_COMPLETIONS,   /* /v1/completions, native FIM via suffix */
  LLM_GHOST_OPENAI_MODE_CHAT,          /* /v1/chat/completions, prompt-engineered FIM */
} LlmGhostOpenAIMode;

G_END_DECLS
```

(The `G_DECLARE_FINAL_TYPE` and constructor are added in Task 4.)

- [ ] **Step 4: Create the internal header**

Create `lib/llmghost-openai-backend-internal.h`:

```c
#pragma once

/* Testing-only internal API. NOT installed. Pure request-body builders,
 * chat-response cleanup, and response extraction for direct unit testing. */

#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-openai-backend.h"

G_BEGIN_DECLS

char *_llm_ghost_openai_build_completions_body (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature);

char *_llm_ghost_openai_build_chat_body        (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature);

/* Trim, unwrap a single ``` fence, then truncate at the first newline.
 * NULL-safe; always returns a newly-allocated string (possibly ""). */
char *_llm_ghost_openai_clean_chat_completion  (const char *raw);

/* Pull the completion text from a parsed response @root. For CHAT, cleans
 * via the above. Returns "" for no/empty choices; NULL + @error when the
 * body carries an API error object. */
char *_llm_ghost_openai_extract_completion     (JsonNode           *root,
                                                LlmGhostOpenAIMode  mode,
                                                GError            **error);

G_END_DECLS
```

- [ ] **Step 5: Implement the pure functions**

Create `lib/llmghost-openai-backend.c`:

```c
#include "llmghost-openai-backend.h"
#include "llmghost-openai-backend-internal.h"

#include <string.h>

#define CHAT_SYSTEM_PROMPT \
  "You are a code completion engine. Output only the code that belongs " \
  "between the given PREFIX and SUFFIX. No explanations, no markdown " \
  "fences, no repetition of the prefix or suffix."

/* ---- request body builders ---------------------------------------------- */

static char *
finish_builder (JsonBuilder *b)
{
  JsonGenerator *gen = json_generator_new ();
  json_generator_set_root (gen, json_builder_get_root (b));
  char *out = json_generator_to_data (gen, NULL);
  g_object_unref (gen);
  g_object_unref (b);
  return out;
}

static void
add_stop_newline (JsonBuilder *b)
{
  json_builder_set_member_name (b, "stop");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "\n");
  json_builder_end_array (b);
}

char *
_llm_ghost_openai_build_completions_body (const char *model,
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
  add_stop_newline (b);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);

  json_builder_end_object (b);
  return finish_builder (b);
}

static void
add_message (JsonBuilder *b, const char *role, const char *content)
{
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, role);
  json_builder_set_member_name (b, "content");
  json_builder_add_string_value (b, content);
  json_builder_end_object (b);
}

char *
_llm_ghost_openai_build_chat_body (const char *model,
                                   const char *prefix,
                                   const char *suffix,
                                   guint       max_tokens,
                                   double      temperature)
{
  char *user = g_strdup_printf ("<PREFIX>%s</PREFIX>\n<SUFFIX>%s</SUFFIX>",
                                prefix ? prefix : "", suffix ? suffix : "");

  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);

  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, model ? model : "");

  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  add_message (b, "system", CHAT_SYSTEM_PROMPT);
  add_message (b, "user", user);
  json_builder_end_array (b);

  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, max_tokens);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, temperature);
  add_stop_newline (b);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);

  json_builder_end_object (b);
  g_free (user);
  return finish_builder (b);
}

/* ---- response cleanup --------------------------------------------------- */

char *
_llm_ghost_openai_clean_chat_completion (const char *raw)
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

/* ---- response extraction ------------------------------------------------ */

char *
_llm_ghost_openai_extract_completion (JsonNode           *root,
                                      LlmGhostOpenAIMode  mode,
                                      GError            **error)
{
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "openai: malformed response");
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
          msg = json_object_get_string_member (obj, "error");
        }
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "openai: %s", msg ? msg : "(error)");
      return NULL;
    }

  if (!json_object_has_member (obj, "choices"))
    return g_strdup ("");

  JsonArray *choices = json_object_get_array_member (obj, "choices");
  if (choices == NULL || json_array_get_length (choices) == 0)
    return g_strdup ("");

  JsonObject *choice = json_array_get_object_element (choices, 0);

  if (mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS)
    {
      const char *text = json_object_has_member (choice, "text")
                             ? json_object_get_string_member (choice, "text")
                             : "";
      return g_strdup (text ? text : "");
    }

  const char *content = "";
  if (json_object_has_member (choice, "message"))
    {
      JsonObject *m = json_object_get_object_member (choice, "message");
      if (m != NULL && json_object_has_member (m, "content"))
        content = json_object_get_string_member (m, "content");
    }
  return _llm_ghost_openai_clean_chat_completion (content);
}
```

- [ ] **Step 6: Add the source + public header to the library**

In `lib/meson.build`: add `'llmghost-openai-backend.c',` to `llmghost_sources`, and add `'llmghost-openai-backend.h',` to `llmghost_headers` (this one IS installed; the `-internal.h` is not).

- [ ] **Step 7: Build and run**

Run: `meson compile -C build && meson test -C build openai-body -v`
Expected: builds clean; `openai-body` reports `OK`, 7 subtests pass.

- [ ] **Step 8: Confirm the whole unit suite passes**

Run: `meson test -C build --suite unit -v`
Expected: all unit tests pass (`fim-tokens`, `ollama-body`, `fake-backend`, `mock-backend`, `ghost-accept`, `http-util`, `openai-body`).

- [ ] **Step 9: Commit**

```bash
git add lib/llmghost-openai-backend.h lib/llmghost-openai-backend-internal.h \
        lib/llmghost-openai-backend.c tests/test-openai-body.c lib/meson.build \
        tests/meson.build
git commit -m "feat: OpenAI request builders + response extraction (pure)

Two body-builders (native-FIM completions, prompt-FIM chat), chat
cleanup (fence-strip + single-line), and a response extractor, all pure
and unit-tested. GObject wrapper follows.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: OpenAI backend GObject + iface + constructor

Wrap the pure functions in a `LlmGhostBackend` that drives `llmghost-http-util`.

**Files:**
- Modify: `lib/llmghost-openai-backend.h` (add type macro + constructor)
- Modify: `lib/llmghost-openai-backend.c` (add GObject + iface + constructor)
- Modify: `lib/llmghost.h`

- [ ] **Step 1: Extend the public header**

In `lib/llmghost-openai-backend.h`, between the enum and `G_END_DECLS`, add:

```c
#define LLM_GHOST_TYPE_OPENAI_BACKEND (llm_ghost_openai_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostOpenAIBackend, llm_ghost_openai_backend,
                      LLM_GHOST, OPENAI_BACKEND, GObject)

/**
 * llm_ghost_openai_backend_new:
 * @base_url: API base, e.g. "https://api.openai.com/v1". NULL/"" →
 *            $LLMGHOST_OPENAI_BASE_URL or the OpenAI default.
 * @model:    model id. NULL/"" → $LLMGHOST_OPENAI_MODEL or "" (server default).
 * @api_key:  bearer token. NULL/"" → $LLMGHOST_OPENAI_API_KEY or no auth.
 * @mode:     COMPLETIONS (native FIM) or CHAT (prompt FIM). Overridden by
 *            $LLMGHOST_OPENAI_MODE ("chat"/"completions") when set.
 */
LlmGhostBackend *llm_ghost_openai_backend_new (const char         *base_url,
                                              const char         *model,
                                              const char         *api_key,
                                              LlmGhostOpenAIMode  mode);
```

- [ ] **Step 2: Add the GObject implementation**

In `lib/llmghost-openai-backend.c`, add the libsoup + util includes after the existing includes:

```c
#include <libsoup/soup.h>
#include "llmghost-http-util.h"
```

Then append the following (struct, type, request path, constructor, lifecycle):

```c
/* ---- type --------------------------------------------------------------- */

#define DEFAULT_BASE_URL     "https://api.openai.com/v1"
#define DEFAULT_MAX_TOKENS   64
#define DEFAULT_TEMPERATURE  0.2
#define REQUEST_TIMEOUT_SEC  30

struct _LlmGhostOpenAIBackend
{
  GObject             parent_instance;

  SoupSession        *session;
  char               *base_url;
  char               *model;
  char               *api_key;     /* NULL = no auth */
  LlmGhostOpenAIMode  mode;
  guint               max_tokens;
  double              temperature;
};

static void llm_ghost_openai_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostOpenAIBackend, llm_ghost_openai_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_openai_backend_iface_init))

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
  GTask              *task  = G_TASK (user_data);
  LlmGhostOpenAIMode  mode  = (LlmGhostOpenAIMode) GPOINTER_TO_INT (g_task_get_task_data (task));
  GError             *error = NULL;

  JsonNode *root = _llm_ghost_http_post_json_finish (result, &error);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  char *out = _llm_ghost_openai_extract_completion (root, mode, &error);
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
openai_request (LlmGhostBackend     *backend,
                const char          *prefix,
                const char          *suffix,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  LlmGhostOpenAIBackend *self = LLM_GHOST_OPENAI_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_task_data (task, GINT_TO_POINTER (self->mode), NULL);

  gboolean comp = (self->mode == LLM_GHOST_OPENAI_MODE_COMPLETIONS);
  char *url  = join_url (self->base_url, comp ? "completions" : "chat/completions");
  char *body = comp
    ? _llm_ghost_openai_build_completions_body (self->model, prefix, suffix,
                                                self->max_tokens, self->temperature)
    : _llm_ghost_openai_build_chat_body (self->model, prefix, suffix,
                                         self->max_tokens, self->temperature);

  _llm_ghost_http_post_json_async (self->session, url, self->api_key, body,
                                   cancellable, on_http_done, task);
  g_free (url);
}

static char *
openai_request_finish (LlmGhostBackend *backend, GAsyncResult *result, GError **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_openai_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = openai_request;
  iface->request_finish = openai_request_finish;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llm_ghost_openai_backend_finalize (GObject *object)
{
  LlmGhostOpenAIBackend *self = LLM_GHOST_OPENAI_BACKEND (object);
  g_clear_object  (&self->session);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->model,    g_free);
  g_clear_pointer (&self->api_key,  g_free);
  G_OBJECT_CLASS (llm_ghost_openai_backend_parent_class)->finalize (object);
}

static void
llm_ghost_openai_backend_class_init (LlmGhostOpenAIBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_openai_backend_finalize;
}

static void
llm_ghost_openai_backend_init (LlmGhostOpenAIBackend *self)
{
  self->session     = soup_session_new ();
  soup_session_set_timeout (self->session, REQUEST_TIMEOUT_SEC);
  self->max_tokens  = DEFAULT_MAX_TOKENS;
  self->temperature = DEFAULT_TEMPERATURE;
  self->mode        = LLM_GHOST_OPENAI_MODE_CHAT;
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

static LlmGhostOpenAIMode
resolve_mode (LlmGhostOpenAIMode fallback)
{
  const char *e = g_getenv ("LLMGHOST_OPENAI_MODE");
  if (e == NULL || *e == '\0')
    return fallback;
  if (g_ascii_strcasecmp (e, "completions") == 0)
    return LLM_GHOST_OPENAI_MODE_COMPLETIONS;
  if (g_ascii_strcasecmp (e, "chat") == 0)
    return LLM_GHOST_OPENAI_MODE_CHAT;
  g_printerr ("llmghost: unknown LLMGHOST_OPENAI_MODE '%s'; using default\n", e);
  return fallback;
}

LlmGhostBackend *
llm_ghost_openai_backend_new (const char         *base_url,
                              const char         *model,
                              const char         *api_key,
                              LlmGhostOpenAIMode  mode)
{
  LlmGhostOpenAIBackend *self = g_object_new (LLM_GHOST_TYPE_OPENAI_BACKEND, NULL);

  self->base_url = pick (base_url, "LLMGHOST_OPENAI_BASE_URL", DEFAULT_BASE_URL);
  self->model    = pick (model,    "LLMGHOST_OPENAI_MODEL",    "");
  self->api_key  = pick_nullable (api_key, "LLMGHOST_OPENAI_API_KEY");
  self->mode     = resolve_mode (mode);

  return LLM_GHOST_BACKEND (self);
}
```

- [ ] **Step 3: Add to the umbrella header**

In `lib/llmghost.h`, add (keeping the list alphabetical-ish, after the ollama include):

```c
#include "llmghost-openai-backend.h"
```

- [ ] **Step 4: Build and verify everything still passes**

Run: `meson compile -C build && meson test -C build`
Expected: builds clean; all suites green — the `unit` suite now includes `openai-body` + `http-util`, and `controller` (gui) still passes. The new GObject compiles and links (constructor + iface present).

- [ ] **Step 5: Commit**

```bash
git add lib/llmghost-openai-backend.h lib/llmghost-openai-backend.c lib/llmghost.h
git commit -m "feat: LlmGhostOpenAIBackend GObject over llmghost-http-util

Implements LlmGhostBackend with COMPLETIONS/CHAT modes, optional Bearer
auth, and constructor + LLMGHOST_OPENAI_* env config. Drives the shared
HTTP helper and the pure builders/extractor.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Demo backend selector

Make the new backend reachable end-to-end via `LLMGHOST_BACKEND=openai`.

**Files:**
- Modify: `tests/demo.c`

- [ ] **Step 1: Add the selector**

In `tests/demo.c`, replace the single-backend construction block (the lines from `const char *host_env = ...` down through the `if (tokens_env ...)` block that builds and configures the Ollama backend) with a selector that builds either backend. Concretely, replace this span:

```c
  const char *host_env   = g_getenv ("LLMGHOST_OLLAMA_HOST");
  const char *port_env   = g_getenv ("LLMGHOST_OLLAMA_PORT");
  const char *model_env  = g_getenv ("LLMGHOST_OLLAMA_MODEL");
  const char *tokens_env = g_getenv ("LLMGHOST_OLLAMA_TOKENS");

  guint16 port = 0;
  if (port_env != NULL && *port_env != '\0')
    {
      gint64 v = g_ascii_strtoll (port_env, NULL, 10);
      if (v > 0 && v < 65536)
        port = (guint16) v;
    }

  LlmGhostBackend *backend = llm_ghost_ollama_backend_new (host_env, port, model_env);

  if (tokens_env != NULL && *tokens_env != '\0')
    {
      const LlmGhostFimTokens *toks = llm_ghost_fim_tokens_lookup_builtin (tokens_env);
      if (toks != NULL)
        llm_ghost_ollama_backend_set_fim_tokens (LLM_GHOST_OLLAMA_BACKEND (backend), toks);
      else
        g_printerr ("llmghost-demo: unknown FIM token set %s; using default (Qwen)\n",
                    tokens_env);
    }
```

with:

```c
  const char *which = g_getenv ("LLMGHOST_BACKEND");   /* "openai" | "ollama" (default) */
  LlmGhostBackend *backend;

  if (which != NULL && g_ascii_strcasecmp (which, "openai") == 0)
    {
      /* base/model/key/mode all read from LLMGHOST_OPENAI_* by the ctor. */
      backend = llm_ghost_openai_backend_new (NULL, NULL, NULL,
                                              LLM_GHOST_OPENAI_MODE_CHAT);
      gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (OpenAI)");
    }
  else
    {
      const char *host_env   = g_getenv ("LLMGHOST_OLLAMA_HOST");
      const char *port_env   = g_getenv ("LLMGHOST_OLLAMA_PORT");
      const char *model_env  = g_getenv ("LLMGHOST_OLLAMA_MODEL");
      const char *tokens_env = g_getenv ("LLMGHOST_OLLAMA_TOKENS");

      guint16 port = 0;
      if (port_env != NULL && *port_env != '\0')
        {
          gint64 v = g_ascii_strtoll (port_env, NULL, 10);
          if (v > 0 && v < 65536)
            port = (guint16) v;
        }

      backend = llm_ghost_ollama_backend_new (host_env, port, model_env);

      if (tokens_env != NULL && *tokens_env != '\0')
        {
          const LlmGhostFimTokens *toks = llm_ghost_fim_tokens_lookup_builtin (tokens_env);
          if (toks != NULL)
            llm_ghost_ollama_backend_set_fim_tokens (LLM_GHOST_OLLAMA_BACKEND (backend), toks);
          else
            g_printerr ("llmghost-demo: unknown FIM token set %s; using default (Qwen)\n",
                        tokens_env);
        }
    }
```

- [ ] **Step 2: Update the demo's header comment**

In the top doc-comment of `tests/demo.c`, add a line under the env-var list:

```c
 *   LLMGHOST_BACKEND        (default ollama; set "openai" for the OpenAI backend)
 *   LLMGHOST_OPENAI_BASE_URL / _MODEL / _API_KEY / _MODE  (OpenAI backend config)
```

- [ ] **Step 3: Build and confirm the suite is still green**

Run: `meson compile -C build && meson test -C build`
Expected: demo builds (it links the new symbols); all tests green. (The demo itself can't be run headless — building + linking is the check here.)

- [ ] **Step 4: Commit**

```bash
git add tests/demo.c
git commit -m "feat: demo LLMGHOST_BACKEND selector (ollama|openai)

Run the demo against the OpenAI backend with LLMGHOST_BACKEND=openai;
defaults to Ollama. Config via LLMGHOST_OPENAI_* env vars.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Documentation + full verification

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Mark the OpenAI backend done in NOTES.md**

In `NOTES.md`, under `## Phase 6 — cloud LLM provider backends`, find the `### Suggested order` item 1 (`LlmGhostOpenAIBackend first ...`). Immediately under the `## Phase 6` heading (before `Three families...`), add a status note:

```markdown
**OpenAI-compatible backend landed 2026-06-04.** `LlmGhostOpenAIBackend`
supports `/v1/completions` (native FIM via `suffix`) and
`/v1/chat/completions` (prompt-FIM), config via constructor +
`LLMGHOST_OPENAI_{BASE_URL,MODEL,API_KEY,MODE}`, optional Bearer auth.
Built on the new shared `llmghost-http-util` (extracted from the Ollama
backend). Reachable in the demo via `LLMGHOST_BACKEND=openai`. Covered by
the `openai-body` and `http-util` unit suites. Still deferred: GSettings
UI, libsecret key storage, SSE streaming, Mistral/Claude backends.
```

- [ ] **Step 2: Commit**

```bash
git add NOTES.md
git commit -m "docs: note OpenAI-compatible backend in NOTES.md

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

- [ ] **Step 3: Full verification**

Run: `meson test -C build`
Expected: all binaries green — unit suite is now `fim-tokens`, `ollama-body`, `fake-backend`, `mock-backend`, `ghost-accept`, `http-util` (4 subtests), `openai-body` (7 subtests); gui suite `controller` (9 subtests).

Run: `meson compile -C build`
Expected: clean (demo + library targets build).

---

## Self-review

**Spec coverage:**
- HTTP-util extraction + division of labor → Task 1 (util) + Task 2 (Ollama refactor). ✓
- COMPLETIONS body (`prompt`+`suffix`+`stop:["\n"]`) → Task 3 `_build_completions_body` + test. ✓
- CHAT body (system+user messages, prefix/suffix embed) → Task 3 `_build_chat_body` + test. ✓
- `clean_chat_completion` (trim → unfence → truncate-at-newline) → Task 3 impl + 8 cases. ✓
- Response extraction (`choices[0].text` / `message.content`, error object, empty) → Task 3 `_extract_completion` + 4 tests. ✓
- Mode enum, constructor, env config, optional Bearer, URL join → Task 4. ✓
- Defaults (cloud OpenAI base, chat mode, empty model, NULL key) → Task 4 `pick`/`resolve_mode`/`init`/`new`. ✓
- In-process SoupServer test (content-type, auth iff key, 200/500/malformed) → Task 1 `test-http-util`. ✓
- Demo reachability via `LLMGHOST_BACKEND` → Task 5. ✓
- Installed vs non-installed headers (public `.h` installed; `-internal.h` + `http-util.h` not) → Tasks 1, 3, 4 meson steps. ✓
- NOTES update → Task 6. ✓

**Placeholder scan:** No TBD/TODO; every code step is complete. ✓

**Type/name consistency:** `_llm_ghost_http_post_json_{async,finish}` identical across header (T1) + Ollama caller (T2) + OpenAI caller (T4); `_llm_ghost_openai_build_completions_body` / `_build_chat_body` / `_clean_chat_completion` / `_extract_completion` identical across internal header (T3), impl (T3), tests (T3), and the GObject caller (T4); `LlmGhostOpenAIMode` / `LLM_GHOST_OPENAI_MODE_{COMPLETIONS,CHAT}` identical across public header (T3/T4), internal header (T3), tests (T3), impl (T4); constructor signature `llm_ghost_openai_backend_new(base_url, model, api_key, mode)` identical in header (T4) + impl (T4) + demo (T5). `_llm_ghost_http_post_json_async` ownership of `json_body` honored at every call site (no double-free). ✓
```
1. llmghost-http-util + loopback test  → verify: meson test http-util green
2. Refactor Ollama onto the util       → verify: meson test ollama-body controller green
3. OpenAI pure fns + unit tests        → verify: meson test --suite unit green
4. OpenAI GObject + ctor               → verify: meson test green (all)
5. Demo selector                       → verify: meson compile + meson test green
6. Docs + full run                     → verify: meson test green end-to-end
```
