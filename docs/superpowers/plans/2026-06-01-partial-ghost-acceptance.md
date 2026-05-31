# Partial Ghost-Text Acceptance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Right=accept-next-char and Ctrl+Right=accept-next-word to `LlmGhostController`, alongside the existing Tab=accept-all / Esc=dismiss.

**Architecture:** A single `accept_ghost_prefix(self, n_bytes)` helper inserts the first N bytes of the current ghost (under the existing `inserting_acceptance` guard), keeps the remainder visible, and re-renders. Two pure UTF-8 boundary functions compute the char/word span; they're exposed via a testing-only internal header. `accept_ghost` (Tab) becomes a thin wrapper over the same helper.

**Tech Stack:** C (gnu11), GLib/GObject, GTK 3, meson/ninja, `g_test`, Xvfb.

---

## Background for the implementer

You're working in a C/GObject/GTK3 library built with **meson**. Build: `meson compile -C build`. Test: `meson test -C build <name> -v`. The `unit` suite is display-free; the `gui` suite (the `controller` test) runs under `xvfb-run` automatically. Ninja auto-regenerates when a `meson.build` changes.

All work is in `lib/llmghost-controller.c` (plus a new internal header) and the test files. **Do not** touch the backend, overlay, or plugin layers.

Key facts about `lib/llmghost-controller.c` (verify with Read before editing — line numbers are approximate):
- Struct field `char *current_ghost` holds the currently-displayed suggestion; `guint inserting_acceptance : 1` is a guard bit.
- `restart_request(self)` returns early when `self->inserting_acceptance` is set — this is how buffer edits during acceptance avoid kicking off a new request.
- `show_ghost_at_cursor(self)` re-checks `cursor_safe_for_ghost` and (re)positions+shows the overlay using `current_ghost`. `hide_ghost(self)` clears `current_ghost` and hides the overlay.
- The existing full-accept is `static void accept_ghost(LlmGhostController *self)` (around line 540): it `g_steal_pointer`s `current_ghost`, inserts it at the cursor wrapped in `inserting_acceptance = TRUE/FALSE`, frees it, and hides the overlay.
- `on_view_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)` (around line 562) returns `GDK_EVENT_PROPAGATE` immediately if `!self->overlay_visible`, then `switch (event->keyval)` handles `GDK_KEY_Tab`/`GDK_KEY_KP_Tab` → `accept_ghost` and `GDK_KEY_Escape` → cancel+hide, `default` → propagate. It includes `<gdk/gdkkeysyms.h>`.
- The forward declarations block near the top lists the static helpers (e.g. `static void accept_ghost (LlmGhostController *self);`).

The test harness in `tests/test-controller.c` (from the test-suite work) provides: `Fixture` (toplevel window + scrolled window + `GtkTextView` + mock backend with response `"abc"` + controller at 30ms debounce), `pump(ms)`, `fixture_new`/`fixture_free`, `buf(f)`, `buffer_text(f)`, `find_ghost_overlay(f)`, `ghost_visible(f)`, and `send_key(f, keyval)` (which builds a `GDK_KEY_PRESS` event and emits `key-press-event`). The mock backend (`tests/mock-backend.h`) is deferred: `mock_backend_set_response`, `mock_backend_complete_pending`, `mock_backend_request_count`. `mock_backend_sources` is defined in `tests/meson.build`.

---

## File structure

| File | Responsibility |
|------|----------------|
| `lib/llmghost-controller-internal.h` | **New.** Non-installed; declares the two pure boundary functions for testing. |
| `lib/llmghost-controller.c` | **Modify.** Add boundary fns + `accept_ghost_prefix`; refactor `accept_ghost`; add Right/KP_Right key cases. |
| `tests/test-ghost-accept.c` | **New.** Unit tests for the boundary functions. |
| `tests/test-controller.c` | **Modify.** Add `send_key_mod` + 3 partial-accept gui tests. |
| `tests/meson.build` | **Modify.** Register `test-ghost-accept` (unit suite). |

---

## Task 1: Boundary functions + internal header + unit tests

Real red→green: the unit test references functions that don't exist yet (link error), then we add them.

**Files:**
- Create: `tests/test-ghost-accept.c`
- Create: `lib/llmghost-controller-internal.h`
- Modify: `lib/llmghost-controller.c` (add the two functions + include the header)
- Modify: `tests/meson.build`

- [ ] **Step 1: Write the failing unit test**

Create `tests/test-ghost-accept.c`:

```c
#include <glib.h>
#include "llmghost-controller-internal.h"

static void
test_next_char_len (void)
{
  g_assert_cmpuint (_llm_ghost_controller_next_char_len ("abc"), ==, 1);
  g_assert_cmpuint (_llm_ghost_controller_next_char_len (""),    ==, 0);
  g_assert_cmpuint (_llm_ghost_controller_next_char_len (NULL),  ==, 0);
  /* "é" is U+00E9 → 2 bytes in UTF-8 */
  g_assert_cmpuint (_llm_ghost_controller_next_char_len ("\xC3\xA9x"), ==, 2);
}

static void
test_next_word_len (void)
{
  /* leading whitespace + word run, stop before punctuation */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("  foo_bar(x)"), ==, 9); /* "  foo_bar" */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("(x)"),          ==, 1); /* "("        */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("x)"),           ==, 1); /* "x"        */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len (")"),            ==, 1); /* ")"        */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("foo bar"),      ==, 3); /* "foo"      */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len (" bar"),         ==, 4); /* " bar"     */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("a1_b="),        ==, 4); /* "a1_b"     */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len (""),             ==, 0);
  g_assert_cmpuint (_llm_ghost_controller_next_word_len (NULL),           ==, 0);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ghost-accept/next-char-len", test_next_char_len);
  g_test_add_func ("/ghost-accept/next-word-len", test_next_word_len);
  return g_test_run ();
}
```

- [ ] **Step 2: Register the test and confirm it fails**

Append to `tests/meson.build`:

```meson
test_ghost_accept = executable(
  'test-ghost-accept',
  'test-ghost-accept.c',
  dependencies: [llmghost_dep],
  install: false,
)
test('ghost-accept', test_ghost_accept, suite: 'unit')
```

Run: `meson compile -C build`
Expected: failure — either `llmghost-controller-internal.h: No such file or directory` (compile) or `undefined reference to _llm_ghost_controller_next_char_len` (link). Either is the acceptable red state.

- [ ] **Step 3: Create the internal header**

Create `lib/llmghost-controller-internal.h`:

```c
#pragma once

/* Testing-only internal API. NOT installed. Exposes the pure ghost-acceptance
 * boundary helpers for direct unit testing. */

#include <glib.h>

G_BEGIN_DECLS

gsize _llm_ghost_controller_next_char_len (const char *ghost);
gsize _llm_ghost_controller_next_word_len (const char *ghost);

G_END_DECLS
```

- [ ] **Step 4: Implement the two functions**

In `lib/llmghost-controller.c`, add the internal-header include next to the existing includes at the top (after `#include "llmghost-overlay.h"`):

```c
#include "llmghost-controller-internal.h"
```

Then add the two function definitions. Put them in the "request flow" / helpers area — a natural spot is just above `accept_ghost` (before the `/* ---- key handling ---- */` section). They are non-`static` (declared in the internal header):

```c
/* ---- ghost-acceptance boundary helpers (exposed for tests) -------------- */

gsize
_llm_ghost_controller_next_char_len (const char *ghost)
{
  if (ghost == NULL || *ghost == '\0')
    return 0;
  return (gsize) (g_utf8_next_char (ghost) - ghost);
}

gsize
_llm_ghost_controller_next_word_len (const char *ghost)
{
  if (ghost == NULL || *ghost == '\0')
    return 0;

  const char *p = ghost;
  while (*p != '\0' && g_unichar_isspace (g_utf8_get_char (p)))   /* leading whitespace */
    p = g_utf8_next_char (p);

  if (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isalnum (c) || c == '_')                      /* run of word chars */
        {
          while (*p != '\0')
            {
              gunichar w = g_utf8_get_char (p);
              if (!g_unichar_isalnum (w) && w != '_')
                break;
              p = g_utf8_next_char (p);
            }
        }
      else                                                        /* single punctuation char */
        {
          p = g_utf8_next_char (p);
        }
    }

  return (gsize) (p - ghost);
}
```

- [ ] **Step 5: Build and run**

Run: `meson compile -C build && meson test -C build ghost-accept -v`
Expected: builds clean; `ghost-accept` reports `OK`, 2 subtests pass.

- [ ] **Step 6: Confirm the rest of the unit suite still passes**

Run: `meson test -C build --suite unit -v`
Expected: all unit tests pass (`fim-tokens`, `ollama-body`, `fake-backend`, `mock-backend`, `ghost-accept`).

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-controller-internal.h lib/llmghost-controller.c \
        tests/test-ghost-accept.c tests/meson.build
git commit -m "feat: add ghost-acceptance boundary helpers

Pure UTF-8 next-char and next-word span functions for partial
ghost-text acceptance, with unit tests.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `accept_ghost_prefix` helper + refactor `accept_ghost`

This refactor is behavior-preserving for Tab/Esc; the existing controller gui tests are the safety net.

**Files:**
- Modify: `lib/llmghost-controller.c` (add `accept_ghost_prefix`, its forward declaration; rewrite `accept_ghost`)

- [ ] **Step 1: Read the current `accept_ghost` and the forward-declaration block**

Run: open `lib/llmghost-controller.c`; locate the `static void accept_ghost (LlmGhostController *self);` forward declaration and the `accept_ghost` definition (around line 540).

- [ ] **Step 2: Add the forward declaration**

In the forward-declarations block near the top, add (next to the existing `static void accept_ghost (...)` line):

```c
static void     accept_ghost_prefix      (LlmGhostController *self, gsize n_bytes);
```

- [ ] **Step 3: Replace the `accept_ghost` definition**

Replace the entire existing `accept_ghost` function body with the new helper plus a thin `accept_ghost`. The current function looks like this (verify before replacing):

```c
static void
accept_ghost (LlmGhostController *self)
{
  if (self->view == NULL || self->current_ghost == NULL)
    return;

  char *text = g_steal_pointer (&self->current_ghost);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  self->inserting_acceptance = TRUE;
  gtk_text_buffer_insert_at_cursor (buffer, text, -1);
  self->inserting_acceptance = FALSE;

  g_free (text);

  if (self->overlay_visible)
    {
      gtk_widget_hide (GTK_WIDGET (self->overlay));
      self->overlay_visible = FALSE;
    }
}
```

Replace it with:

```c
/* Accept the first n_bytes of the current ghost: insert that slice, keep the
 * remainder visible (re-rendered at the advanced cursor), or hide if nothing
 * is left. The inserting_acceptance guard stops the buffer insert from
 * triggering restart_request (which would cancel + hide + re-fetch). */
static void
accept_ghost_prefix (LlmGhostController *self, gsize n_bytes)
{
  if (self->view == NULL || self->current_ghost == NULL || n_bytes == 0)
    return;

  n_bytes = MIN (n_bytes, strlen (self->current_ghost));

  char *accepted = g_strndup (self->current_ghost, n_bytes);
  char *rest     = g_strdup (self->current_ghost + n_bytes);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  self->inserting_acceptance = TRUE;
  gtk_text_buffer_insert_at_cursor (buffer, accepted, -1);
  self->inserting_acceptance = FALSE;
  g_free (accepted);

  g_clear_pointer (&self->current_ghost, g_free);

  if (*rest != '\0')
    {
      self->current_ghost = rest;          /* take ownership */
      show_ghost_at_cursor (self);
    }
  else
    {
      g_free (rest);
      hide_ghost (self);
    }
}

static void
accept_ghost (LlmGhostController *self)
{
  if (self->current_ghost != NULL)
    accept_ghost_prefix (self, strlen (self->current_ghost));
}
```

(`strlen`/`MIN` are already available — `<string.h>` semantics via GLib and the `MIN` macro from glib. The file already uses `strchr` and `g_ascii_isspace`, so string.h is in scope through the existing includes; if the compiler complains about `strlen`, add `#include <string.h>` near the top.)

- [ ] **Step 4: Build and verify existing behavior is preserved**

Run: `meson compile -C build && meson test -C build controller -v`
Expected: builds clean; all 6 existing controller subtests still pass (especially `/controller/tab-accepts` and `/controller/escape-dismisses`, which now exercise the refactored path).

- [ ] **Step 5: Commit**

```bash
git add lib/llmghost-controller.c
git commit -m "refactor: route ghost acceptance through accept_ghost_prefix

Make accept_ghost a thin wrapper over a new accept_ghost_prefix(n_bytes)
helper that keeps any remainder visible. Behaviour-preserving for Tab.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Right / Ctrl+Right key handling + gui tests

TDD: add the gui tests first (they fail because Right isn't handled), then wire up the key cases.

**Files:**
- Modify: `tests/test-controller.c` (add `send_key_mod` + 3 tests + register them)
- Modify: `lib/llmghost-controller.c` (`on_view_key_press`)

- [ ] **Step 1: Add `send_key_mod` and refactor `send_key`**

In `tests/test-controller.c`, replace the existing `send_key` function:

```c
static gboolean
send_key (Fixture *f, guint keyval)
{
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.keyval = keyval;
  GdkWindow *win = gtk_widget_get_window (GTK_WIDGET (f->view));
  ev->key.window = win != NULL ? g_object_ref (win) : NULL;
  gboolean handled = FALSE;
  g_signal_emit_by_name (f->view, "key-press-event", ev, &handled);
  gdk_event_free (ev);
  return handled;
}
```

with:

```c
static gboolean
send_key_mod (Fixture *f, guint keyval, guint state)
{
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.keyval = keyval;
  ev->key.state  = state;
  GdkWindow *win = gtk_widget_get_window (GTK_WIDGET (f->view));
  ev->key.window = win != NULL ? g_object_ref (win) : NULL;
  gboolean handled = FALSE;
  g_signal_emit_by_name (f->view, "key-press-event", ev, &handled);
  gdk_event_free (ev);
  return handled;
}

static gboolean
send_key (Fixture *f, guint keyval)
{
  return send_key_mod (f, keyval, 0);
}
```

- [ ] **Step 2: Add the three gui test functions**

In `tests/test-controller.c`, add these after `test_escape_dismisses` (they reuse the existing fixture + mock pattern):

```c
static void
test_right_accepts_char (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));   /* response "abc" */
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Right));
  char *t1 = buffer_text (f);
  g_assert_cmpstr (t1, ==, "fa");
  g_free (t1);
  g_assert_true (ghost_visible (f));   /* "bc" remains */

  g_assert_true (send_key (f, GDK_KEY_Right));
  char *t2 = buffer_text (f);
  g_assert_cmpstr (t2, ==, "fab");
  g_free (t2);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key (f, GDK_KEY_Right));
  char *t3 = buffer_text (f);
  g_assert_cmpstr (t3, ==, "fabc");
  g_free (t3);
  g_assert_false (ghost_visible (f));  /* nothing left */
  fixture_free (f);
}

static void
test_ctrl_right_accepts_word (void)
{
  Fixture *f = fixture_new ();
  mock_backend_set_response (MOCK_BACKEND (f->backend), "foo bar");
  gtk_text_buffer_insert_at_cursor (buf (f), "x", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t1 = buffer_text (f);
  g_assert_cmpstr (t1, ==, "xfoo");
  g_free (t1);
  g_assert_true (ghost_visible (f));   /* " bar" remains */

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t2 = buffer_text (f);
  g_assert_cmpstr (t2, ==, "xfoo bar");
  g_free (t2);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}

static void
test_ctrl_right_punctuation (void)
{
  Fixture *f = fixture_new ();
  mock_backend_set_response (MOCK_BACKEND (f->backend), "ab(c");
  gtk_text_buffer_insert_at_cursor (buf (f), "x", -1);
  pump (SETTLE_MS);
  mock_backend_complete_pending (MOCK_BACKEND (f->backend));
  pump (SETTLE_MS);
  g_assert_true (ghost_visible (f));

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t1 = buffer_text (f);
  g_assert_cmpstr (t1, ==, "xab");      /* word run "ab" */
  g_free (t1);

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t2 = buffer_text (f);
  g_assert_cmpstr (t2, ==, "xab(");     /* single punctuation "(" */
  g_free (t2);

  g_assert_true (send_key_mod (f, GDK_KEY_Right, GDK_CONTROL_MASK));
  char *t3 = buffer_text (f);
  g_assert_cmpstr (t3, ==, "xab(c");    /* word run "c" */
  g_free (t3);
  g_assert_false (ghost_visible (f));
  fixture_free (f);
}
```

- [ ] **Step 3: Register the three tests in `main`**

In `tests/test-controller.c`'s `main`, add after the existing `g_test_add_func` lines:

```c
  g_test_add_func ("/controller/right-accepts-char",     test_right_accepts_char);
  g_test_add_func ("/controller/ctrl-right-accepts-word", test_ctrl_right_accepts_word);
  g_test_add_func ("/controller/ctrl-right-punctuation",  test_ctrl_right_punctuation);
```

- [ ] **Step 4: Run the gui tests and confirm the new ones fail**

Run: `meson compile -C build && meson test -C build controller -v`
Expected: the 6 original tests pass; the 3 new tests FAIL — `send_key` returns `GDK_EVENT_PROPAGATE` (FALSE) for Right because the controller doesn't handle it yet, so `g_assert_true (send_key (...))` fails. (If `send_key` returns TRUE unexpectedly, stop and investigate.)

- [ ] **Step 5: Add the Right / KP_Right cases to `on_view_key_press`**

In `lib/llmghost-controller.c`, locate `on_view_key_press`. Just after the early `if (!self->overlay_visible) return GDK_EVENT_PROPAGATE;` and before the `switch`, compute the masked modifiers:

```c
  guint mods = event->state & gtk_accelerator_get_default_mod_mask ();
```

Then add these cases to the `switch (event->keyval)` (alongside the existing Tab/Escape cases, before `default`):

```c
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      if (mods == 0)
        {
          accept_ghost_prefix (self, _llm_ghost_controller_next_char_len (self->current_ghost));
          return GDK_EVENT_STOP;
        }
      if (mods == GDK_CONTROL_MASK)
        {
          accept_ghost_prefix (self, _llm_ghost_controller_next_word_len (self->current_ghost));
          return GDK_EVENT_STOP;
        }
      return GDK_EVENT_PROPAGATE;
```

(`gtk_accelerator_get_default_mod_mask` and `GDK_CONTROL_MASK` come from gtk/gdk headers already included via `gtk.h`/`gdkkeysyms.h`. The internal-header functions are already declared from Task 1's include.)

- [ ] **Step 6: Run the gui tests — all green**

Run: `meson compile -C build && meson test -C build controller -v`
Expected: all 9 controller subtests pass (6 original + 3 new).

- [ ] **Step 7: Commit**

```bash
git add lib/llmghost-controller.c tests/test-controller.c
git commit -m "feat: accept ghost text by char (Right) and word (Ctrl+Right)

Right accepts the next character, Ctrl+Right the next word; the
remainder stays visible. Tab/Esc unchanged. Other modifiers propagate.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Documentation

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Document the keybindings**

In `NOTES.md`, find the Phase 1 bullet that mentions "Tab/Esc" (in the
"## Phase 1 — done" section, the `LlmGhostController` bullet). Update that
bullet to read:

```markdown
- `LlmGhostController` — debounce, in-flight cancellation, Tab/Esc,
  Right=accept-next-char, Ctrl+Right=accept-next-word (word = leading
  whitespace + word-char run, else one punctuation char),
  cursor-position-aware repositioning, scroll repositioning,
  cursor-safe-for-ghost suppression, trailing-whitespace strip
```

- [ ] **Step 2: Commit**

```bash
git add NOTES.md
git commit -m "docs: note partial-accept keybindings in NOTES.md

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Full verification

- [ ] `meson test -C build` → all binaries green (6 unit subtests across the existing files + 2 `ghost-accept` + 9 `controller`).
- [ ] `meson compile -C build` → clean (demo + plugin targets unaffected).

---

## Self-review

**Spec coverage:**
- Word-boundary definition (leading WS + word run, else 1 punct) → Task 1 `_llm_ghost_controller_next_word_len` + unit tests. ✓
- Char boundary (UTF-8 next char) → Task 1 `_llm_ghost_controller_next_char_len` + unit test (incl. multi-byte). ✓
- `accept_ghost_prefix` + `accept_ghost` refactor → Task 2. ✓
- Right=char, Ctrl+Right=word, modifier masking, KP_Right, propagate-on-other-modifiers → Task 3 Step 5. ✓
- Remainder stays visible / clears on last chunk → Task 2 helper + Task 3 gui tests assert it. ✓
- Internal header, not installed → Task 1 Step 3 (and not added to `llmghost_headers`). ✓
- Unit tests + 3 gui tests (char, word, punctuation) → Tasks 1 & 3. ✓
- Verification commands → per-task + Full verification. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"; every code step shows complete code. ✓

**Type/name consistency:** `_llm_ghost_controller_next_char_len` / `_llm_ghost_controller_next_word_len` identical across header (Task 1), impl (Task 1), and call sites (Task 3); `accept_ghost_prefix(self, n_bytes)` signature identical in forward decl (Task 2), definition (Task 2), and calls (Task 3); `send_key_mod(f, keyval, state)` defined and used consistently (Task 3). Return type `gsize` consistent with the `n_bytes` parameter. ✓
```
1. Boundary fns + internal header + unit tests → verify: meson test --suite unit green
2. accept_ghost_prefix + accept_ghost refactor → verify: meson test controller green (Tab/Esc unchanged)
3. Right/Ctrl+Right + gui tests                → verify: meson test controller green (9 subtests)
4. Docs + full run                             → verify: meson test green end-to-end
```
