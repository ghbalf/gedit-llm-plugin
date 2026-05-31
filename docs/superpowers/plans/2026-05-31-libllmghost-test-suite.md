# libllmghost Test Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the first automated `g_test` suite for `libllmghost` — pure-logic, async-contract, and headless-GTK-controller tests — wired into `meson test`, before the Phase 5/6 backend work.

**Architecture:** GLib `g_test` cases registered with meson's native `test()`, split into a display-free `unit` suite and an Xvfb-wrapped `gui` suite. A testing-only internal header exposes the one `static` pure function we test directly. A purpose-built instrumented mock backend makes debounce-coalescing and in-flight cancellation observable.

**Tech Stack:** C (gnu11), GLib/GObject, GTK 3, json-glib, meson/ninja, Xvfb.

---

## Background for the implementer

You are working in a C/GObject library built with **meson**. Key facts:

- Build dir already exists: build with `meson compile -C build`. Ninja auto-regenerates when any `meson.build` changes, so newly-added source files are picked up automatically.
- Run tests with `meson test -C build <name> -v` (the `-v` streams the test's stdout). Run a whole suite with `meson test -C build --suite unit -v`.
- The library target is `llmghost_dep` (declared in `lib/meson.build`); linking against it also adds `lib/` to the include path and pulls in gtk/glib/gio/soup/json deps. Test executables just need `dependencies: [llmghost_dep]`.
- `g_test` assertion macros: `g_assert_true`, `g_assert_false`, `g_assert_nonnull`, `g_assert_null`, `g_assert_cmpstr`, `g_assert_cmpint`, `g_assert_no_error`. Use these (NOT bare `g_assert`, which compiles out under `G_DISABLE_ASSERT`).
- The FIM token builtins have these **exact** values (from `lib/llmghost-fim-tokens.c`), which the tests assert against:
  - Qwen: name `"Qwen"`, prefix `<|fim_prefix|>`, suffix `<|fim_suffix|>`, middle `<|fim_middle|>`, stops `<|endoftext|>`, `<|fim_pad|>`, `<|im_end|>`.
  - StarCoder: name `"StarCoder"`, prefix `<fim_prefix>`.
  - DeepSeek: name `"DeepSeek"`, prefix `<｜fim▁begin｜>` (Unicode U+FF5C / U+2581 — copy it verbatim, do not retype with ASCII pipes).

Some tasks add **characterization tests** over already-working code: the meaningful failure they guard against is a mis-stated assertion, so "see it fail" for those is "the test binary doesn't build/isn't registered yet, then it builds and passes." Tasks that introduce new code (the internal-header refactor, the mock backend, the controller behaviour) have genuine red→green transitions; those are called out per task.

---

## File structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-ollama-backend-internal.h` | **New.** Non-installed header exposing the pure request-body builder for tests. |
| `lib/llmghost-ollama-backend.c` | **Modify.** Rename/de-`static` the builder, include the internal header. |
| `tests/test-fim-tokens.c` | **New.** Pure value-type tests. |
| `tests/test-ollama-body.c` | **New.** Pure JSON request-body tests. |
| `tests/test-fake-backend.c` | **New.** Async backend-contract test via `GMainLoop`. |
| `tests/mock-backend.h` / `tests/mock-backend.c` | **New.** Instrumented `LlmGhostBackend` test double. |
| `tests/test-mock-backend.c` | **New.** Headless self-test for the mock (cancel/complete counting). |
| `tests/test-controller.c` | **New.** Headless GTK controller tests (behavior + sanity coords). |
| `tests/meson.build` | **Modify.** Register the test executables. |
| `meson.build` (top) | **Modify.** Detect `xvfb-run`, add the default `xvfb` test setup. |

---

## Task 1: FIM-tokens pure tests

**Files:**
- Create: `tests/test-fim-tokens.c`
- Modify: `tests/meson.build`

- [ ] **Step 1: Write the test file**

Create `tests/test-fim-tokens.c`:

```c
#include <glib.h>
#include "llmghost-fim-tokens.h"

static void
test_new_copies_fields (void)
{
  const char *stops[] = { "<|endoftext|>", NULL };
  LlmGhostFimTokens *t =
      llm_ghost_fim_tokens_new ("X", "<p>", "<s>", "<m>",
                                (const char * const *) stops);
  g_assert_nonnull (t);
  g_assert_cmpstr (t->name,       ==, "X");
  g_assert_cmpstr (t->prefix_tok, ==, "<p>");
  g_assert_cmpstr (t->suffix_tok, ==, "<s>");
  g_assert_cmpstr (t->middle_tok, ==, "<m>");
  g_assert_nonnull (t->stop_tokens);
  g_assert_cmpstr (t->stop_tokens[0], ==, "<|endoftext|>");
  g_assert_null   (t->stop_tokens[1]);
  llm_ghost_fim_tokens_free (t);
}

static void
test_copy_is_independent (void)
{
  const char *stops[] = { "S", NULL };
  LlmGhostFimTokens *orig =
      llm_ghost_fim_tokens_new ("orig", "p", "s", "m",
                                (const char * const *) stops);
  LlmGhostFimTokens *dup = llm_ghost_fim_tokens_copy (orig);
  llm_ghost_fim_tokens_free (orig);   /* free original; copy must survive */

  g_assert_nonnull (dup);
  g_assert_cmpstr (dup->name,          ==, "orig");
  g_assert_cmpstr (dup->prefix_tok,    ==, "p");
  g_assert_cmpstr (dup->stop_tokens[0], ==, "S");
  g_assert_null   (dup->stop_tokens[1]);
  llm_ghost_fim_tokens_free (dup);
}

static void
test_free_null_is_safe (void)
{
  llm_ghost_fim_tokens_free (NULL);   /* must not crash */
  g_assert_null (llm_ghost_fim_tokens_copy (NULL));
}

static void
test_builtins_present_and_distinct (void)
{
  const LlmGhostFimTokens *q = llm_ghost_fim_tokens_qwen ();
  const LlmGhostFimTokens *s = llm_ghost_fim_tokens_starcoder ();
  const LlmGhostFimTokens *d = llm_ghost_fim_tokens_deepseek ();

  g_assert_nonnull (q);
  g_assert_nonnull (s);
  g_assert_nonnull (d);
  g_assert_cmpstr (q->name, ==, "Qwen");
  g_assert_cmpstr (s->name, ==, "StarCoder");
  g_assert_cmpstr (d->name, ==, "DeepSeek");
  g_assert_cmpstr (q->prefix_tok, ==, "<|fim_prefix|>");
  g_assert_cmpstr (s->prefix_tok, ==, "<fim_prefix>");
  /* distinct sentinels across families */
  g_assert_cmpstr (q->prefix_tok, !=, s->prefix_tok);
  g_assert_cmpstr (q->prefix_tok, !=, d->prefix_tok);
}

static void
test_builtins_list_is_null_terminated (void)
{
  const LlmGhostFimTokens * const *all = llm_ghost_fim_tokens_builtins ();
  g_assert_nonnull (all);
  guint n = 0;
  while (all[n] != NULL)
    n++;
  g_assert_cmpint (n, ==, 3);
  g_assert_cmpstr (all[0]->name, ==, "Qwen");
  g_assert_cmpstr (all[1]->name, ==, "StarCoder");
  g_assert_cmpstr (all[2]->name, ==, "DeepSeek");
}

static void
test_lookup_builtin_case_insensitive (void)
{
  g_assert_cmpstr (llm_ghost_fim_tokens_lookup_builtin ("qwen")->name, ==, "Qwen");
  g_assert_cmpstr (llm_ghost_fim_tokens_lookup_builtin ("QWEN")->name, ==, "Qwen");
  g_assert_cmpstr (llm_ghost_fim_tokens_lookup_builtin ("Qwen")->name, ==, "Qwen");
  g_assert_null   (llm_ghost_fim_tokens_lookup_builtin ("nope"));
  g_assert_null   (llm_ghost_fim_tokens_lookup_builtin (NULL));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fim-tokens/new-copies-fields",     test_new_copies_fields);
  g_test_add_func ("/fim-tokens/copy-independent",      test_copy_is_independent);
  g_test_add_func ("/fim-tokens/free-null-safe",        test_free_null_is_safe);
  g_test_add_func ("/fim-tokens/builtins-distinct",     test_builtins_present_and_distinct);
  g_test_add_func ("/fim-tokens/builtins-list",         test_builtins_list_is_null_terminated);
  g_test_add_func ("/fim-tokens/lookup-case-insensitive", test_lookup_builtin_case_insensitive);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test in meson**

Append to `tests/meson.build`:

```meson
test_fim_tokens = executable(
  'test-fim-tokens',
  'test-fim-tokens.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('fim-tokens', test_fim_tokens, suite: 'unit')
```

- [ ] **Step 3: Build and run**

Run: `meson compile -C build && meson test -C build fim-tokens -v`
Expected: builds clean; test `fim-tokens` reports `OK` with 6 subtests passing.

- [ ] **Step 4: Commit**

```bash
git add tests/test-fim-tokens.c tests/meson.build
git commit -m "test: add FIM-tokens pure unit tests

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Expose and test the Ollama request-body builder

This task has a real red→green: the test references a symbol that doesn't exist yet (link error), then we expose it.

**Files:**
- Create: `tests/test-ollama-body.c`
- Create: `lib/llmghost-ollama-backend-internal.h`
- Modify: `lib/llmghost-ollama-backend.c:36-98` (rename + de-`static`)
- Modify: `tests/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-ollama-body.c`:

```c
#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-fim-tokens.h"
#include "llmghost-ollama-backend-internal.h"

static JsonObject *
parse_object (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonNode *root = json_parser_get_root (parser);
  g_assert_true (JSON_NODE_HOLDS_OBJECT (root));
  JsonObject *obj = json_object_ref (json_node_get_object (root));
  g_object_unref (parser);
  return obj;
}

static void
test_body_top_level_fields (void)
{
  char *body = _llm_ghost_ollama_build_request_body (
      "my-model", llm_ghost_fim_tokens_qwen (), "int main", "}", 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"), ==, "my-model");
  g_assert_true  (json_object_get_boolean_member (obj, "raw"));
  g_assert_false (json_object_get_boolean_member (obj, "stream"));
  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==,
                   "<|fim_prefix|>int main<|fim_suffix|>}<|fim_middle|>");

  json_object_unref (obj);
  g_free (body);
}

static void
test_body_options_and_stops (void)
{
  char *body = _llm_ghost_ollama_build_request_body (
      "m", llm_ghost_fim_tokens_qwen (), "a", "b", 64, 0.2);
  JsonObject *obj = parse_object (body);
  JsonObject *opts = json_object_get_object_member (obj, "options");

  g_assert_cmpint (json_object_get_int_member (opts, "num_predict"), ==, 64);
  g_assert_true (ABS (json_object_get_double_member (opts, "temperature") - 0.2) < 1e-9);

  JsonArray *stop = json_object_get_array_member (opts, "stop");
  /* newline first, then the Qwen family stops in order */
  g_assert_cmpint (json_array_get_length (stop), ==, 4);
  g_assert_cmpstr (json_array_get_string_element (stop, 0), ==, "\n");
  g_assert_cmpstr (json_array_get_string_element (stop, 1), ==, "<|endoftext|>");
  g_assert_cmpstr (json_array_get_string_element (stop, 2), ==, "<|fim_pad|>");
  g_assert_cmpstr (json_array_get_string_element (stop, 3), ==, "<|im_end|>");

  json_object_unref (obj);
  g_free (body);
}

static void
test_body_null_prefix_suffix (void)
{
  char *body = _llm_ghost_ollama_build_request_body (
      "m", llm_ghost_fim_tokens_qwen (), NULL, NULL, 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==,
                   "<|fim_prefix|><|fim_suffix|><|fim_middle|>");

  json_object_unref (obj);
  g_free (body);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ollama-body/top-level",   test_body_top_level_fields);
  g_test_add_func ("/ollama-body/options-stops", test_body_options_and_stops);
  g_test_add_func ("/ollama-body/null-prefix-suffix", test_body_null_prefix_suffix);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test and confirm it fails to link**

Append to `tests/meson.build`:

```meson
test_ollama_body = executable(
  'test-ollama-body',
  'test-ollama-body.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('ollama-body', test_ollama_body, suite: 'unit')
```

Run: `meson compile -C build`
Expected: **link error** — `undefined reference to _llm_ghost_ollama_build_request_body`. This confirms the symbol isn't exposed yet.

- [ ] **Step 3: Create the internal header**

Create `lib/llmghost-ollama-backend-internal.h`:

```c
#pragma once

/* Testing-only internal API. NOT part of the installed headers — exists so
 * the unit tests can exercise the otherwise-static request-body builder
 * directly. Do not depend on this from library consumers. */

#include <glib.h>
#include "llmghost-fim-tokens.h"

G_BEGIN_DECLS

char *_llm_ghost_ollama_build_request_body (const char              *model,
                                            const LlmGhostFimTokens *tokens,
                                            const char              *prefix,
                                            const char              *suffix,
                                            guint                    num_predict,
                                            double                   temperature);

G_END_DECLS
```

- [ ] **Step 4: Rename/de-`static` the builder**

In `lib/llmghost-ollama-backend.c`, add the include near the top (after the existing includes, around line 5):

```c
#include "llmghost-ollama-backend-internal.h"
```

Then change the function definition at line 36 from:

```c
static char *
build_request_body (const char              *model,
```

to:

```c
char *
_llm_ghost_ollama_build_request_body (const char              *model,
```

And update its one call site inside `ollama_request` (around line 208) from:

```c
  char *body = build_request_body (self->model, self->fim_tokens,
                                   prefix, suffix,
                                   self->num_predict, self->temperature);
```

to:

```c
  char *body = _llm_ghost_ollama_build_request_body (self->model, self->fim_tokens,
                                                     prefix, suffix,
                                                     self->num_predict, self->temperature);
```

No other logic changes.

- [ ] **Step 5: Build and run**

Run: `meson compile -C build && meson test -C build ollama-body -v`
Expected: builds clean; `ollama-body` reports `OK`, 3 subtests pass.

- [ ] **Step 6: Confirm nothing else broke**

Run: `meson test -C build --suite unit -v`
Expected: both `fim-tokens` and `ollama-body` pass.

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-ollama-backend-internal.h lib/llmghost-ollama-backend.c \
        tests/test-ollama-body.c tests/meson.build
git commit -m "test: expose and characterize Ollama request-body builder

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Fake-backend async-contract test

**Files:**
- Create: `tests/test-fake-backend.c`
- Modify: `tests/meson.build`

- [ ] **Step 1: Write the test**

Create `tests/test-fake-backend.c`:

```c
#include <glib.h>
#include <gio/gio.h>
#include "llmghost-backend.h"
#include "llmghost-fake-backend.h"

typedef struct {
  GMainLoop *loop;
  char      *result;
  GError    *error;
} Ctx;

static void
on_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Ctx *c = user_data;
  c->result = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                                res, &c->error);
  g_main_loop_quit (c->loop);
}

static char *
run_one_request (LlmGhostBackend *backend, GError **error)
{
  Ctx c = { g_main_loop_new (NULL, FALSE), NULL, NULL };
  llm_ghost_backend_request (backend, "prefix", "suffix", NULL, on_ready, &c);
  g_main_loop_run (c.loop);
  g_main_loop_free (c.loop);
  if (error != NULL)
    *error = c.error;
  else
    g_clear_error (&c.error);
  return c.result;
}

static void
test_fake_returns_canned (void)
{
  LlmGhostBackend *b = llm_ghost_fake_backend_new ("CANNED");
  GError *error = NULL;
  char *out = run_one_request (b, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "CANNED");
  g_free (out);
  g_object_unref (b);
}

static void
test_fake_default_response (void)
{
  LlmGhostBackend *b = llm_ghost_fake_backend_new (NULL);
  GError *error = NULL;
  char *out = run_one_request (b, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "// hello, ghost!");
  g_free (out);
  g_object_unref (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fake-backend/returns-canned",   test_fake_returns_canned);
  g_test_add_func ("/fake-backend/default-response",  test_fake_default_response);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test**

Append to `tests/meson.build`:

```meson
test_fake_backend = executable(
  'test-fake-backend',
  'test-fake-backend.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('fake-backend', test_fake_backend, suite: 'unit')
```

- [ ] **Step 3: Build and run**

Run: `meson compile -C build && meson test -C build fake-backend -v`
Expected: `OK`, 2 subtests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/test-fake-backend.c tests/meson.build
git commit -m "test: cover fake-backend async contract

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Instrumented mock backend + self-test

The mock is new code with real logic (cancel/complete bookkeeping), so it gets its own headless self-test before the controller depends on it.

**Files:**
- Create: `tests/mock-backend.h`
- Create: `tests/mock-backend.c`
- Create: `tests/test-mock-backend.c`
- Modify: `tests/meson.build`

- [ ] **Step 1: Write the mock header**

Create `tests/mock-backend.h`:

```c
#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

#define MOCK_TYPE_BACKEND (mock_backend_get_type ())
G_DECLARE_FINAL_TYPE (MockBackend, mock_backend, MOCK, BACKEND, GObject)

/* Deferred-by-default test double for LlmGhostBackend. Requests are parked
 * until mock_backend_complete_pending() is called, so tests control timing.
 * A request whose GCancellable fires is counted and completes with
 * G_IO_ERROR_CANCELLED instead. */
LlmGhostBackend *mock_backend_new            (void);
void             mock_backend_set_response   (MockBackend *self, const char *text);
guint            mock_backend_request_count  (MockBackend *self);
guint            mock_backend_cancel_count   (MockBackend *self);
void             mock_backend_complete_pending (MockBackend *self);

G_END_DECLS
```

- [ ] **Step 2: Write the mock implementation**

Create `tests/mock-backend.c`:

```c
#include "mock-backend.h"

typedef struct {
  GTask        *task;
  GCancellable *cancellable;   /* reffed, or NULL */
  gulong        cancel_id;     /* g_cancellable_connect id, or 0 */
  MockBackend  *self;          /* unowned backref */
} Pending;

struct _MockBackend
{
  GObject  parent_instance;
  char    *response;
  guint    request_count;
  guint    cancel_count;
  GList   *pending;            /* Pending* */
};

static void mock_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MockBackend, mock_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                mock_backend_iface_init))

static void
pending_free (Pending *p)
{
  g_clear_object (&p->cancellable);
  g_free (p);
}

/* Fires (synchronously, from g_cancellable_cancel) when a parked request's
 * cancellable is cancelled. Do NOT call g_cancellable_disconnect here — that
 * deadlocks when invoked from within the cancelled handler. */
static void
on_cancelled (GCancellable *cancellable, gpointer user_data)
{
  (void) cancellable;
  Pending *p = user_data;
  MockBackend *self = p->self;

  GList *link = g_list_find (self->pending, p);
  if (link == NULL)
    return;   /* already completed */

  self->pending = g_list_delete_link (self->pending, link);
  self->cancel_count++;
  p->cancel_id = 0;   /* fired; nothing left to disconnect */

  g_task_return_new_error (p->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "mock: cancelled");
  g_object_unref (p->task);
  pending_free (p);
}

static void
mock_request (LlmGhostBackend     *backend,
              const char          *prefix,
              const char          *suffix,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
  (void) prefix;
  (void) suffix;
  MockBackend *self = MOCK_BACKEND (backend);
  self->request_count++;

  Pending *p = g_new0 (Pending, 1);
  p->self = self;
  p->task = g_task_new (self, cancellable, callback, user_data);
  if (cancellable != NULL)
    {
      p->cancellable = g_object_ref (cancellable);
      p->cancel_id = g_cancellable_connect (cancellable,
                                            G_CALLBACK (on_cancelled), p, NULL);
    }
  self->pending = g_list_append (self->pending, p);
}

static char *
mock_request_finish (LlmGhostBackend  *backend,
                     GAsyncResult     *result,
                     GError          **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
mock_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = mock_request;
  iface->request_finish = mock_request_finish;
}

void
mock_backend_set_response (MockBackend *self, const char *text)
{
  g_clear_pointer (&self->response, g_free);
  self->response = g_strdup (text);
}

guint
mock_backend_request_count (MockBackend *self)
{
  return self->request_count;
}

guint
mock_backend_cancel_count (MockBackend *self)
{
  return self->cancel_count;
}

void
mock_backend_complete_pending (MockBackend *self)
{
  GList *snapshot = self->pending;
  self->pending = NULL;

  for (GList *l = snapshot; l != NULL; l = l->next)
    {
      Pending *p = l->data;
      if (p->cancel_id != 0 && p->cancellable != NULL)
        g_cancellable_disconnect (p->cancellable, p->cancel_id);
      g_task_return_pointer (p->task,
                             g_strdup (self->response ? self->response : "MOCK"),
                             g_free);
      g_object_unref (p->task);
      pending_free (p);
    }
  g_list_free (snapshot);
}

static void
mock_backend_finalize (GObject *object)
{
  MockBackend *self = MOCK_BACKEND (object);
  for (GList *l = self->pending; l != NULL; l = l->next)
    {
      Pending *p = l->data;
      if (p->cancel_id != 0 && p->cancellable != NULL)
        g_cancellable_disconnect (p->cancellable, p->cancel_id);
      g_task_return_new_error (p->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "mock: backend finalized");
      g_object_unref (p->task);
      pending_free (p);
    }
  g_clear_pointer (&self->pending, g_list_free);
  g_clear_pointer (&self->response, g_free);
  G_OBJECT_CLASS (mock_backend_parent_class)->finalize (object);
}

static void
mock_backend_class_init (MockBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = mock_backend_finalize;
}

static void
mock_backend_init (MockBackend *self)
{
  (void) self;
}

LlmGhostBackend *
mock_backend_new (void)
{
  return LLM_GHOST_BACKEND (g_object_new (MOCK_TYPE_BACKEND, NULL));
}
```

- [ ] **Step 3: Write the mock self-test**

Create `tests/test-mock-backend.c`:

```c
#include <glib.h>
#include <gio/gio.h>
#include "mock-backend.h"

typedef struct {
  GMainLoop *loop;
  char      *result;
  GError    *error;
  gboolean   done;
} Ctx;

static void
on_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Ctx *c = user_data;
  c->result = llm_ghost_backend_request_finish (LLM_GHOST_BACKEND (source),
                                                res, &c->error);
  c->done = TRUE;
  if (c->loop != NULL)
    g_main_loop_quit (c->loop);
}

/* Pump the default context briefly so parked GTask completions dispatch. */
static gboolean stop_loop (gpointer loop) { g_main_loop_quit (loop); return G_SOURCE_REMOVE; }
static void
pump (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (ms, stop_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_free (loop);
}

static void
test_complete_returns_response (void)
{
  LlmGhostBackend *b = mock_backend_new ();
  mock_backend_set_response (MOCK_BACKEND (b), "HELLO");
  Ctx c = { 0 };

  llm_ghost_backend_request (b, "p", "s", NULL, on_ready, &c);
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (b)), ==, 1);
  g_assert_false (c.done);   /* deferred: nothing yet */

  mock_backend_complete_pending (MOCK_BACKEND (b));
  pump (20);                 /* let the GTask callback dispatch */

  g_assert_true (c.done);
  g_assert_no_error (c.error);
  g_assert_cmpstr (c.result, ==, "HELLO");
  g_free (c.result);
  g_object_unref (b);
}

static void
test_cancel_counts_and_errors (void)
{
  LlmGhostBackend *b = mock_backend_new ();
  GCancellable *cancellable = g_cancellable_new ();
  Ctx c = { 0 };

  llm_ghost_backend_request (b, "p", "s", cancellable, on_ready, &c);
  g_cancellable_cancel (cancellable);   /* fires on_cancelled synchronously */

  g_assert_cmpint (mock_backend_cancel_count (MOCK_BACKEND (b)), ==, 1);
  pump (20);                            /* dispatch the CANCELLED completion */

  g_assert_true (c.done);
  g_assert_error (c.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null (c.result);
  g_clear_error (&c.error);
  g_object_unref (cancellable);
  g_object_unref (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mock-backend/complete-returns-response", test_complete_returns_response);
  g_test_add_func ("/mock-backend/cancel-counts-and-errors",  test_cancel_counts_and_errors);
  return g_test_run ();
}
```

- [ ] **Step 4: Register in meson**

Append to `tests/meson.build`:

```meson
mock_backend_sources = files('mock-backend.c')

test_mock_backend = executable(
  'test-mock-backend',
  ['test-mock-backend.c', mock_backend_sources],
  dependencies: [llmghost_dep],
  install: false,
)
test('mock-backend', test_mock_backend, suite: 'unit')
```

- [ ] **Step 5: Build and run**

Run: `meson compile -C build && meson test -C build mock-backend -v`
Expected: `OK`, 2 subtests pass.

- [ ] **Step 6: Commit**

```bash
git add tests/mock-backend.h tests/mock-backend.c tests/test-mock-backend.c tests/meson.build
git commit -m "test: add instrumented mock backend with self-test

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Controller tests (gui — behavior + sanity coords) + Xvfb wiring

**Files:**
- Create: `tests/test-controller.c`
- Modify: `meson.build` (top) — add Xvfb detection + default test setup
- Modify: `tests/meson.build` — register the gui test (guarded on `xvfb-run`)

- [ ] **Step 1: Add Xvfb detection and default test setup**

In the top-level `meson.build`, after the dependency block (after the `peas_dep = ...` line, before `llmghost_inc = ...`), add:

```meson
# Headless GTK tests run under a virtual framebuffer. Making this the default
# test setup lets `meson test` "just work"; display-free tests are wrapped too
# (harmless). The gui suite is only registered when xvfb-run is present.
xvfb_run = find_program('xvfb-run', required: false)
if xvfb_run.found()
  add_test_setup('xvfb', exe_wrapper: [xvfb_run, '-a'], is_default: true)
endif
```

- [ ] **Step 2: Write the controller test**

Create `tests/test-controller.c`:

```c
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "llmghost-controller.h"
#include "llmghost-overlay.h"
#include "mock-backend.h"

#define DEBOUNCE_MS 30
#define SETTLE_MS   90   /* > debounce, leaves room for idle dispatch */

/* ---- harness helpers ----------------------------------------------------- */

static gboolean stop_loop (gpointer loop) { g_main_loop_quit (loop); return G_SOURCE_REMOVE; }

static void
pump (guint ms)
{
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (ms, stop_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_free (loop);
}

typedef struct {
  GtkWidget          *window;
  GtkWidget          *scrolled;
  GtkTextView        *view;
  LlmGhostBackend    *backend;   /* MockBackend */
  LlmGhostController *controller;
} Fixture;

static Fixture *
fixture_new (void)
{
  Fixture *f = g_new0 (Fixture, 1);
  f->window   = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (f->window), 300, 200);
  f->scrolled = gtk_scrolled_window_new (NULL, NULL);
  f->view     = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_container_add (GTK_CONTAINER (f->scrolled), GTK_WIDGET (f->view));
  gtk_container_add (GTK_CONTAINER (f->window), f->scrolled);

  f->backend = mock_backend_new ();
  mock_backend_set_response (MOCK_BACKEND (f->backend), "abc");
  f->controller = llm_ghost_controller_new (f->view, f->backend);
  llm_ghost_controller_set_debounce_ms (f->controller, DEBOUNCE_MS);

  gtk_widget_show_all (f->window);
  pump (SETTLE_MS);   /* realize + initial allocate */
  return f;
}

static void
fixture_free (Fixture *f)
{
  g_object_unref (f->controller);
  g_object_unref (f->backend);
  gtk_widget_destroy (f->window);
  pump (10);
  g_free (f);
}

static GtkTextBuffer *buf (Fixture *f) { return gtk_text_view_get_buffer (f->view); }

static char *
buffer_text (Fixture *f)
{
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds (buf (f), &s, &e);
  return gtk_text_buffer_get_text (buf (f), &s, &e, FALSE);
}

static LlmGhostOverlay *
find_ghost_overlay (Fixture *f)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (f->view));
  LlmGhostOverlay *found = NULL;
  for (GList *l = children; l != NULL; l = l->next)
    if (LLM_GHOST_IS_OVERLAY (l->data))
      {
        found = l->data;
        break;
      }
  g_list_free (children);
  return found;
}

static gboolean
ghost_visible (Fixture *f)
{
  LlmGhostOverlay *o = find_ghost_overlay (f);
  return o != NULL && gtk_widget_get_visible (GTK_WIDGET (o));
}

static gboolean
send_key (Fixture *f, guint keyval)
{
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.keyval = keyval;
  GdkWindow *win = gtk_widget_get_window (GTK_WIDGET (f->view));
  ev->key.window = win != NULL ? g_object_ref (win) : NULL;
  gboolean handled = FALSE;
  g_signal_emit_by_name (f->view, "key-press-event", &ev->key, &handled);
  gdk_event_free (ev);
  return handled;
}

/* ---- tests --------------------------------------------------------------- */

static void
test_debounce_coalesces (void)
{
  Fixture *f = fixture_new ();
  /* Four edits with no main-loop iteration between them: the debounce timer
   * cannot fire, so only the final pending timer survives → one request. */
  gtk_text_buffer_insert_at_cursor (buf (f), "a", -1);
  gtk_text_buffer_insert_at_cursor (buf (f), "b", -1);
  gtk_text_buffer_insert_at_cursor (buf (f), "c", -1);
  gtk_text_buffer_insert_at_cursor (buf (f), "d", -1);

  pump (SETTLE_MS);

  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 1);
  fixture_free (f);
}

static void
test_cancel_on_new_input (void)
{
  Fixture *f = fixture_new ();

  gtk_text_buffer_insert_at_cursor (buf (f), "a", -1);
  pump (SETTLE_MS);   /* request #1 issued, parked */
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 1);

  gtk_text_buffer_insert_at_cursor (buf (f), "b", -1);   /* cancels #1 */
  pump (SETTLE_MS);   /* request #2 issued */

  g_assert_cmpint (mock_backend_cancel_count  (MOCK_BACKEND (f->backend)), ==, 1);
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 2);

  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));
  fixture_free (f);
}

static void
test_tab_accepts (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Tab));

  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "fabc");
  g_free (text);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_escape_dismisses (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Escape));

  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "f");   /* buffer untouched */
  g_free (text);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_midline_suppression (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_set_text (buf (f), "xy", -1);
  GtkTextIter start;
  gtk_text_buffer_get_start_iter (buf (f), &start);
  gtk_text_buffer_place_cursor (buf (f), &start);   /* cursor before "xy" */

  pump (SETTLE_MS);

  /* Rest-of-line is non-whitespace → no completion requested, no ghost. */
  g_assert_cmpint (mock_backend_request_count (MOCK_BACKEND (f->backend)), ==, 0);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_sanity_coords_after_scroll (void)
{
  Fixture *f = fixture_new ();
  gtk_widget_set_size_request (f->scrolled, 220, 120);

  /* Many lines, ending in an empty (ghost-safe) final line. */
  GString *s = g_string_new (NULL);
  for (int i = 0; i < 60; i++)
    g_string_append_printf (s, "line %d\n", i);
  gtk_text_buffer_set_text (buf (f), s->str, -1);
  g_string_free (s, TRUE);

  /* Park the cursor on the empty last line and scroll it into view. */
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buf (f), &end);
  gtk_text_buffer_place_cursor (buf (f), &end);
  GtkTextMark *insert = gtk_text_buffer_get_insert (buf (f));
  gtk_text_view_scroll_to_mark (f->view, insert, 0.0, TRUE, 0.0, 0.5);
  pump (SETTLE_MS);

  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);

  LlmGhostOverlay *overlay = find_ghost_overlay (f);
  g_assert_nonnull (overlay);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (overlay)));

  /* Precondition: we actually scrolled far enough for the bug to manifest. */
  GtkAdjustment *vadj =
      gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (f->view));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark (buf (f), &cur, insert);
  gint line_y = 0, line_h = 0;
  gtk_text_view_get_line_yrange (f->view, &cur, &line_y, &line_h);
  g_assert_cmpint (line_h, >, 0);
  g_assert_cmpint ((gint) gtk_adjustment_get_value (vadj), >, line_h);

  /* Cursor line's window-y vs. the overlay's allocated y must agree within
   * one line height. A buffer-vs-window coordinate mixup (the Phase-3 bug)
   * would differ by the scroll offset, which we just asserted exceeds line_h. */
  GdkRectangle r;
  gtk_text_view_get_iter_location (f->view, &cur, &r);
  gint win_x = 0, win_y = 0;
  gtk_text_view_buffer_to_window_coords (f->view, GTK_TEXT_WINDOW_TEXT,
                                         r.x, r.y, &win_x, &win_y);

  GtkAllocation alloc;
  gtk_widget_get_allocation (GTK_WIDGET (overlay), &alloc);

  g_assert_cmpint (ABS (alloc.y - win_y), <=, line_h);
  fixture_free (f);
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  g_test_add_func ("/controller/debounce-coalesces",   test_debounce_coalesces);
  g_test_add_func ("/controller/cancel-on-new-input",  test_cancel_on_new_input);
  g_test_add_func ("/controller/tab-accepts",          test_tab_accepts);
  g_test_add_func ("/controller/escape-dismisses",     test_escape_dismisses);
  g_test_add_func ("/controller/midline-suppression",  test_midline_suppression);
  g_test_add_func ("/controller/sanity-coords",        test_sanity_coords_after_scroll);
  return g_test_run ();
}
```

- [ ] **Step 3: Register the gui test (guarded on xvfb-run)**

Append to `tests/meson.build`:

```meson
if xvfb_run.found()
  test_controller = executable(
    'test-controller',
    ['test-controller.c', mock_backend_sources],
    dependencies: [llmghost_dep],
    install: false,
  )
  test('controller', test_controller, suite: 'gui')
endif
```

- [ ] **Step 4: Build and run the gui test**

Run: `meson compile -C build && meson test -C build controller -v`
Expected: `OK`, 6 subtests pass. (Runs under `xvfb-run -a` via the default setup.)

If `/controller/tab-accepts` or `/escape-dismisses` reports `handled == FALSE`, the synthesized key event isn't reaching the handler — re-check that the view is realized (`gtk_widget_get_window` non-NULL after `fixture_new`'s `pump`). If `/controller/sanity-coords` fails the `vadj > line_h` precondition, the view didn't scroll; increase the line count or shrink the scrolled-window size request.

- [ ] **Step 5: Run the whole suite**

Run: `meson test -C build -v`
Expected: all five test binaries (`fim-tokens`, `ollama-body`, `fake-backend`, `mock-backend`, `controller`) pass.

- [ ] **Step 6: Confirm the demo still builds**

Run: `meson compile -C build`
Expected: clean; `tests/llmghost-demo` target unaffected.

- [ ] **Step 7: Commit**

```bash
git add meson.build tests/test-controller.c tests/meson.build
git commit -m "test: add headless controller tests under Xvfb

Behaviour (debounce coalescing, cancellation, Tab-accept, Esc-dismiss,
mid-line suppression) plus a sanity-coordinate guard for the Phase-3
buffer-vs-window regression.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Document the suite in NOTES.md

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Add a testing section**

Insert after the "Build cheat sheet" section's command block in `NOTES.md`:

```markdown
## Tests

`g_test` suite under `tests/`, wired into `meson test`:

- `--suite unit` — display-free: FIM tokens, the Ollama request-body builder
  (via `lib/llmghost-ollama-backend-internal.h`), the fake-backend async
  contract, and the instrumented mock backend.
- `--suite gui` — headless `LlmGhostController` tests under Xvfb (behaviour +
  sanity-coordinate guard for the Phase-3 buffer-vs-window bug). Registered
  only when `xvfb-run` is found; it is the default test setup so plain
  `meson test` runs everything.

```
meson test -C build              # all suites (gui wrapped in xvfb-run -a)
meson test -C build --suite unit # display-free subset
meson test -C build controller -v
```

The mock backend (`tests/mock-backend.{h,c}`) is deferred-by-default so tests
drive completion timing; the production `LlmGhostFakeBackend` can't, since it
completes synchronously and ignores its `GCancellable`.
```

- [ ] **Step 2: Commit**

```bash
git add NOTES.md
git commit -m "docs: note the test suite in NOTES.md

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage:**
- Meson layout + two suites → Tasks 1–5 (`unit` suite Tasks 1–4, `gui` Task 5). ✓
- Xvfb default setup, guarded on `xvfb-run` → Task 5 Step 1/3. ✓
- Internal header for `build_request_body` → Task 2. ✓
- `test-fim-tokens` cases (new/copy/free-null/builtins/list/lookup) → Task 1. ✓
- `test-ollama-body` cases (top-level/options+stops/null-args) → Task 2. ✓
- `test-fake-backend` (canned + default) → Task 3. ✓
- Mock backend (request/cancel counts, deferred completion) → Task 4. ✓
- Controller behaviour (debounce, cancel, Tab, Esc, mid-line) + sanity coords → Task 5. ✓
- Verification commands from the spec → covered in each task's run steps + Task 5 Step 5. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step shows full code. ✓

**Type/name consistency:** `_llm_ghost_ollama_build_request_body` (header Task 2 == call site Task 2 == test Task 2); `mock_backend_*` names identical across header (Task 4), impl (Task 4), self-test (Task 4), controller test (Task 5); `find_ghost_overlay`/`ghost_visible`/`send_key`/`pump`/`fixture_*` defined once in Task 5 and used there. `mock_backend_sources` defined in Task 4, reused in Task 5. ✓
```
1. Pure-logic + builder tests → verify: `meson test --suite unit` green (Tasks 1-2)
2. Async fake-backend test    → verify: `meson test fake-backend` green (Task 3)
3. Mock backend + self-test   → verify: `meson test mock-backend` green (Task 4)
4. Controller under Xvfb      → verify: `meson test controller` green (Task 5)
5. Full run + demo intact     → verify: `meson test` + `meson compile` green (Task 5)
```
