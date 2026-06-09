# Multi-line Ghost Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render Copilot-style multi-line ghost suggestions — first line inline at the cursor, the rest as a block below with real text pushed down — instead of truncating completions at the first newline.

**Architecture:** Three independent changes. (1) A controller-owned `max_lines` cap clamps the suggestion and stays the single source of truth (`current_ghost`), so display == accept. (2) Rendering opens a real gap below the cursor line with a `pixels-below-lines` spacer tag (no buffer mutation → no undo/modified side effects) and floats a second multi-line overlay label into it (Approach C). (3) Backends stop sending the `\n` stop token when not in single-line mode.

**Tech Stack:** C (gnu11), GLib/GObject/GIO, GTK3, libsoup-3, json-glib, meson/ninja, GLib `g_test` (+ Xvfb for the gui suite).

**Reference spec:** `docs/superpowers/specs/2026-06-09-multiline-ghost-design.md`

## Conventions (read before starting)

- Every `.c` starts with `#define G_LOG_DOMAIN "..."` if it logs; match the existing file.
- **Internal headers** (`*-internal.h`) are added to `llmghost_sources` only via their `.c`; never to the installed `llmghost_headers`.
- Known false positives — do NOT "fix": clang-tidy `bugprone-sizeof-expression` on `g_clear_*`/`g_clear_pointer`/`g_clear_object` lines; clangd "unused include" on the `llmghost.h` umbrella; stale clangd diagnostics in test files after signature changes (the meson build is the source of truth).
- Build + test (from repo root):
  - Build: `ninja -C build`
  - Unit suite: `meson test -C build --suite unit`
  - GUI suite (xvfb): `meson test -C build --suite gui`
  - One test: `meson test -C build <name>` (e.g. `ghost-accept`, `controller`, `openai-body`)
  - A failing build counts as a failing step — fix before moving on.
- **Sequencing invariant:** the controller's `max_lines` field defaults to **1** until Task 3. While it is 1, `clamp_ghost_text` reproduces today's single-line behavior, so every intermediate commit keeps `display == accept` and all existing tests stay green. Task 3 flips the default to 8 *and* lands the multi-line renderer in the same commit.

## File Structure

**Modified files**
- `lib/llmghost-controller-internal.h` — declare the two new pure helpers.
- `lib/llmghost-controller.c` — `clamp_ghost_text`/`count_lines`; `max_lines` field + setter; `overlay_block` + spacer tag; extended show/reposition/hide/detach.
- `lib/llmghost-controller.h` — `llm_ghost_controller_set_max_lines` declaration.
- `lib/llmghost-overlay.{c,h}` — multi-line constructor; `set_text` stops truncating.
- `lib/llmghost-openai-backend.c` + `-internal.h` — `single_line` param on builders + field/setter.
- `lib/llmghost-ollama-backend.c` + `-internal.h` — same.
- `lib/llmghost-mistral-backend.c` + `-internal.h` — same.
- `lib/llmghost-backend-factory.c` — read `max_lines`, set `single_line` on the active backend.
- `lib/llmghost-settings.{c,h}` — `llm_ghost_settings_get_max_lines` + default JSON key.
- `plugin/llmghost-plugin.c` — wire `max_lines` → controller.
- `tests/test-ghost-accept.c` — pure clamp/count-lines tests.
- `tests/test-controller.c` — multi-line gui tests; update the single-line regression test.
- `tests/test-openai-body.c`, `tests/test-ollama-body.c`, `tests/test-mistral-body.c` — stop-token tests.
- `tests/test-settings.c` — `max_lines` getter test.
- `NOTES.md` — mark Phase 4 landed.

---

### Task 1: Pure `clamp_ghost_text` + `count_lines` helpers

Replaces the single-line `sanitize_ghost_text` with a line-capping clamp, exposed for unit testing. Behavior stays single-line because the controller's `max_lines` field defaults to 1 in this task.

**Files:**
- Modify: `lib/llmghost-controller-internal.h`
- Modify: `lib/llmghost-controller.c`
- Modify: `tests/test-ghost-accept.c`

- [ ] **Step 1: Declare the helpers in the internal header**

In `lib/llmghost-controller-internal.h`, add before `G_END_DECLS`:

```c

/* Clamp @text to at most @max_lines lines (split on '\n'), then right-trim
 * trailing whitespace and blank lines. Leading whitespace is preserved.
 * Returns a newly-allocated string, or NULL if nothing meaningful remains.
 * @max_lines == 0 is treated as 1. With @max_lines == 1 this reproduces the
 * old single-line "truncate at first newline + right-trim" behavior. */
char *_llm_ghost_controller_clamp_ghost_text (const char *text, guint max_lines);

/* Number of lines in @text (1 + count of '\n'); 0 for NULL/empty. */
guint _llm_ghost_controller_count_lines (const char *text);
```

- [ ] **Step 2: Write the failing tests**

In `tests/test-ghost-accept.c`, add `#include <string.h>` if absent, then add these tests before `main`:

```c
static void
test_clamp_single_line (void)
{
  char *a = _llm_ghost_controller_clamp_ghost_text ("foo\nbar\nbaz", 1);
  g_assert_cmpstr (a, ==, "foo");
  g_free (a);

  /* trailing whitespace right-trimmed; leading preserved */
  char *b = _llm_ghost_controller_clamp_ghost_text ("  hi  ", 1);
  g_assert_cmpstr (b, ==, "  hi");
  g_free (b);
}

static void
test_clamp_multi_line (void)
{
  char *a = _llm_ghost_controller_clamp_ghost_text ("a\nb\nc\nd", 2);
  g_assert_cmpstr (a, ==, "a\nb");
  g_free (a);

  /* fewer lines than the cap → unchanged (modulo trailing trim) */
  char *b = _llm_ghost_controller_clamp_ghost_text ("a\nb\n", 8);
  g_assert_cmpstr (b, ==, "a\nb");
  g_free (b);
}

static void
test_clamp_trailing_blank_lines (void)
{
  char *a = _llm_ghost_controller_clamp_ghost_text ("x\n\n\n", 8);
  g_assert_cmpstr (a, ==, "x");
  g_free (a);
}

static void
test_clamp_empty_is_null (void)
{
  g_assert_null (_llm_ghost_controller_clamp_ghost_text (NULL, 8));
  g_assert_null (_llm_ghost_controller_clamp_ghost_text ("", 8));
  g_assert_null (_llm_ghost_controller_clamp_ghost_text ("   \n  ", 8));
  /* max_lines 0 treated as 1 */
  char *a = _llm_ghost_controller_clamp_ghost_text ("p\nq", 0);
  g_assert_cmpstr (a, ==, "p");
  g_free (a);
}

static void
test_count_lines (void)
{
  g_assert_cmpuint (_llm_ghost_controller_count_lines (NULL),     ==, 0);
  g_assert_cmpuint (_llm_ghost_controller_count_lines (""),       ==, 0);
  g_assert_cmpuint (_llm_ghost_controller_count_lines ("a"),      ==, 1);
  g_assert_cmpuint (_llm_ghost_controller_count_lines ("a\nb"),   ==, 2);
  g_assert_cmpuint (_llm_ghost_controller_count_lines ("a\nb\n"), ==, 3);
}
```

Register them in `main` (add before `return g_test_run ();`):

```c
  g_test_add_func ("/ghost-accept/clamp-single-line",   test_clamp_single_line);
  g_test_add_func ("/ghost-accept/clamp-multi-line",    test_clamp_multi_line);
  g_test_add_func ("/ghost-accept/clamp-trailing-blank", test_clamp_trailing_blank_lines);
  g_test_add_func ("/ghost-accept/clamp-empty-null",    test_clamp_empty_is_null);
  g_test_add_func ("/ghost-accept/count-lines",         test_count_lines);
```

- [ ] **Step 3: Build to verify it fails**

Run: `ninja -C build`
Expected: FAIL — `undefined reference to '_llm_ghost_controller_clamp_ghost_text'` / `_llm_ghost_controller_count_lines`.

- [ ] **Step 4: Implement the helpers and rewire the call sites**

In `lib/llmghost-controller.c`, find the existing `sanitize_ghost_text` function (it truncates at the first `\n` and right-trims) and **replace it entirely** with the two exported helpers:

```c
/* Phase 4: clamp @text to at most @max_lines lines, then right-trim trailing
 * whitespace and blank lines (FIM models emit trailing whitespace; trailing
 * blank lines would open empty gap rows). Leading whitespace is preserved
 * (meaningful indentation). Returns a newly-allocated string, or NULL when
 * nothing meaningful remains. This is the single source of truth: current_ghost
 * holds exactly this string, so the displayed ghost and what accept inserts
 * always agree. */
char *
_llm_ghost_controller_clamp_ghost_text (const char *text, guint max_lines)
{
  if (text == NULL)
    return NULL;
  if (max_lines == 0)
    max_lines = 1;

  char *copy = g_strdup (text);

  /* Cut after the max_lines-th line: find the max_lines-th '\n'. */
  guint nl = 0;
  for (char *q = copy; *q != '\0'; q++)
    if (*q == '\n' && ++nl == max_lines)
      {
        *q = '\0';
        break;
      }

  /* Right-trim trailing whitespace (removes trailing blank lines + spaces). */
  for (char *end = copy + strlen (copy);
       end > copy && g_ascii_isspace ((unsigned char) end[-1]);
       end--)
    end[-1] = '\0';

  if (*copy == '\0')
    {
      g_free (copy);
      return NULL;
    }
  return copy;
}

guint
_llm_ghost_controller_count_lines (const char *text)
{
  if (text == NULL || *text == '\0')
    return 0;
  guint n = 1;
  for (const char *p = text; *p != '\0'; p++)
    if (*p == '\n')
      n++;
  return n;
}
```

Update the **forward declaration**: find `static char *   sanitize_ghost_text       (char *text);` in the forward-declaration block and delete it (the helpers are now declared in the internal header, which is already `#include`d at the top of the file).

Add a `max_lines` field to `struct _LlmGhostController`, after `guint debounce_ms;`:

```c
  guint                max_lines;
```

Default it in `llm_ghost_controller_init`, after `self->debounce_ms = DEFAULT_DEBOUNCE_MS;`:

```c
  self->max_lines   = 1;   /* single-line until the multi-line renderer lands */
```

Now update the two call sites. In `on_completion_ready`, replace:

```c
  text = sanitize_ghost_text (text);  /* consumes text */
  if (text == NULL)
    goto out;
```
with:
```c
  char *clamped = _llm_ghost_controller_clamp_ghost_text (text, self->max_lines);
  g_free (text);
  text = clamped;
  if (text == NULL)
    goto out;
```

In `on_partial_data`, replace:

```c
  /* Same single-line sanitisation as the final result, so the displayed ghost
   * and what Tab-accept inserts agree even mid-stream. */
  char *clean = sanitize_ghost_text (g_strdup (text));
  if (clean == NULL)
    return;
```
with:
```c
  /* Same clamp as the final result, so the displayed ghost and what accept
   * inserts agree even mid-stream. */
  char *clean = _llm_ghost_controller_clamp_ghost_text (text, self->max_lines);
  if (clean == NULL)
    return;
```

- [ ] **Step 5: Build and run the tests**

Run: `ninja -C build && meson test -C build ghost-accept controller -v`
Expected: PASS — the new pure tests pass; the existing gui controller tests are unchanged (still single-line, because `max_lines == 1`).

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-controller-internal.h lib/llmghost-controller.c tests/test-ghost-accept.c
git commit -m "feat(controller): line-capping clamp_ghost_text (single source of truth)"
```

---

### Task 2: Overlay multi-line mode

Adds a multi-line constructor and stops `set_text` from truncating at the first newline. No behavior change yet — the controller still uses the single-line inline overlay with `max_lines == 1`.

**Files:**
- Modify: `lib/llmghost-overlay.h`
- Modify: `lib/llmghost-overlay.c`

- [ ] **Step 1: Declare the multi-line constructor**

In `lib/llmghost-overlay.h`, add after the existing `llm_ghost_overlay_new` declaration:

```c
GtkWidget *llm_ghost_overlay_new_multiline (void);
```

- [ ] **Step 2: Implement it and stop truncating in set_text**

In `lib/llmghost-overlay.c`, refactor `init` so the single-line mode is set by the constructor rather than unconditionally. Replace the line in `llm_ghost_overlay_init`:

```c
  gtk_label_set_single_line_mode (label, TRUE);
```
with:
```c
  gtk_label_set_single_line_mode (label, TRUE);   /* default; cleared by _new_multiline */
```

Add the constructor after `llm_ghost_overlay_new`:

```c
GtkWidget *
llm_ghost_overlay_new_multiline (void)
{
  GtkWidget *w = g_object_new (LLM_GHOST_TYPE_OVERLAY, NULL);
  gtk_label_set_single_line_mode (GTK_LABEL (w), FALSE);
  return w;
}
```

Replace the body of `llm_ghost_overlay_set_text` — remove the first-newline truncation so the multi-line instance renders every line (the single-line instance still collapses to one line via GtkLabel, and the controller only ever passes it a single line):

```c
void
llm_ghost_overlay_set_text (LlmGhostOverlay *self,
                            const char      *text)
{
  g_return_if_fail (LLM_GHOST_IS_OVERLAY (self));

  if (text == NULL || *text == '\0')
    {
      gtk_label_set_markup (GTK_LABEL (self), "");
      return;
    }

  char *escaped = g_markup_escape_text (text, -1);
  char *markup = g_strdup_printf (
      "<span foreground=\"#888888\" style=\"italic\">%s</span>", escaped);

  gtk_label_set_markup (GTK_LABEL (self), markup);

  g_free (markup);
  g_free (escaped);
}
```

(Remove the now-unused `#include <string.h>` only if nothing else in the file uses it — check for other `str*`/`mem*` calls first; leave it if present elsewhere.)

- [ ] **Step 3: Build and run the full suite**

Run: `ninja -C build && meson test -C build`
Expected: PASS — no behavior change; everything green. (The single-line overlay still shows one line because GtkLabel `single_line_mode` is TRUE and the controller passes it single-line text.)

- [ ] **Step 4: Commit**

```bash
git add lib/llmghost-overlay.c lib/llmghost-overlay.h
git commit -m "feat(overlay): multi-line mode + non-truncating set_text"
```

---

### Task 3: Controller multi-line rendering (spacer tag + block overlay)

The core change. Adds the continuation block overlay and the `pixels-below-lines` spacer tag, splits `current_ghost` across the two labels, and flips `max_lines` to 8.

**Files:**
- Modify: `lib/llmghost-controller.c`
- Modify: `tests/test-controller.c`

- [ ] **Step 1: Write/adjust the failing gui tests**

In `tests/test-controller.c`:

First, **update** the existing single-line regression test `test_partial_accept_single_line` (added during SSE streaming) — multi-line is now intended, so a two-line partial should show line 2 in the block and accept both lines. Replace that whole function with:

```c
/* A multi-line partial now renders as a block and accepts in full. */
static void
test_partial_accept_multi_line (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);   /* request now in-flight */

  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "line1\nline2");
  pump (SETTLE_MS);

  g_assert_true (send_key (f, GDK_KEY_Tab));
  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "fline1\nline2");
  g_free (text);
  fixture_free (f);
}
```

And update its registration in `main`: replace

```c
  g_test_add_func ("/controller/partial-accept-single-line", test_partial_accept_single_line);
```
with
```c
  g_test_add_func ("/controller/partial-accept-multi-line", test_partial_accept_multi_line);
```

Add a helper to read the continuation block's text (after the `ghost_text` helper):

```c
static char *
block_text (Fixture *f)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (f->view));
  char *out = g_strdup ("");
  guint seen = 0;
  for (GList *l = children; l != NULL; l = l->next)
    if (LLM_GHOST_IS_OVERLAY (l->data))
      {
        /* Two overlays may be present: inline (single-line) and block
         * (multi-line). The block is the one whose label text contains a
         * newline, or — when the block is a single continuation line — the
         * one that is NOT single-line-mode. Identify by single-line-mode. */
        if (!gtk_label_get_single_line_mode (GTK_LABEL (l->data)))
          {
            g_free (out);
            out = g_strdup (gtk_label_get_text (GTK_LABEL (l->data)));
            seen++;
          }
      }
  g_list_free (children);
  (void) seen;
  return out;
}
```

Add two gui tests before `main`:

```c
static void
test_multiline_splits_inline_and_block (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  mock_backend_emit_partial (MOCK_BACKEND (f->backend), "foo(\n    bar,\n    baz");
  pump (SETTLE_MS);

  g_assert_true (ghost_visible (f));
  char *inline_t = ghost_text (f);
  g_assert_cmpstr (inline_t, ==, "foo(");          /* first line inline */
  g_free (inline_t);

  char *blk = block_text (f);
  g_assert_cmpstr (blk, ==, "    bar,\n    baz");   /* lines 2..N in the block */
  g_free (blk);
  fixture_free (f);
}

static void
test_multiline_cap (void)
{
  Fixture *f = fixture_new ();
  gtk_text_buffer_insert_at_cursor (buf (f), "f", -1);
  pump (SETTLE_MS);
  /* 10 lines, cap is 8 → accept inserts exactly 8 (first line "L1" continues
   * the buffer's "f"). */
  mock_backend_emit_partial (MOCK_BACKEND (f->backend),
                             "L1\nL2\nL3\nL4\nL5\nL6\nL7\nL8\nL9\nL10");
  pump (SETTLE_MS);
  g_assert_true (send_key (f, GDK_KEY_Tab));
  char *text = buffer_text (f);
  g_assert_cmpstr (text, ==, "fL1\nL2\nL3\nL4\nL5\nL6\nL7\nL8");
  g_free (text);
  fixture_free (f);
}
```

Register both in `main`:

```c
  g_test_add_func ("/controller/multiline-split", test_multiline_splits_inline_and_block);
  g_test_add_func ("/controller/multiline-cap",   test_multiline_cap);
```

- [ ] **Step 2: Build and run to verify failure**

Run: `ninja -C build && meson test -C build controller -v`
Expected: FAIL — `test_multiline_splits_inline_and_block` fails (block empty / inline shows full text), `test_multiline_cap` and `test_partial_accept_multi_line` fail (still single-line because `max_lines == 1` and no block renderer). `gtk_label_get_single_line_mode` is declared by GTK — no include needed beyond `<gtk/gtk.h>`.

- [ ] **Step 3: Add the block overlay + spacer-tag fields**

In `lib/llmghost-controller.c`, add to `struct _LlmGhostController`, after `LlmGhostOverlay *overlay;`:

```c
  LlmGhostOverlay     *overlay_block; /* strong; multi-line continuation, child of view */
  GtkTextTag          *spacer_tag;    /* owned by the buffer's tag table; opens the gap */
```

Add to the bitfield group, after `guint overlay_added : 1;`:

```c
  guint                overlay_block_added : 1;
```

In `llm_ghost_controller_init`, after the existing overlay creation, add the block overlay and flip the cap to 8:

```c
  self->overlay_block = LLM_GHOST_OVERLAY (llm_ghost_overlay_new_multiline ());
  g_object_ref_sink (self->overlay_block);
```
and change `self->max_lines = 1;` to:
```c
  self->max_lines   = 8;   /* multi-line cap */
```

In `llm_ghost_controller_dispose`, after `g_clear_object (&self->overlay);`, add:

```c
  g_clear_object (&self->overlay_block);
```

- [ ] **Step 4: Create + apply/clear the spacer tag**

Still in `lib/llmghost-controller.c`. Add a forward declaration near the other rendering forward decls (after `static void     show_ghost_at_cursor      (LlmGhostController *self);`):

```c
static void     clear_spacer              (LlmGhostController *self);
```

In `attach_to_view`, right after `self->connected_buffer = buffer;`, create the tag on the buffer:

```c
  self->spacer_tag = gtk_text_buffer_create_tag (buffer, NULL,
                                                 "pixels-below-lines", 0, NULL);
```

In `detach_from_view`, inside the `if (self->connected_buffer != NULL)` block (before `self->connected_buffer = NULL;`), drop the tag pointer (the tag dies with the buffer's tag table):

```c
      self->spacer_tag = NULL;
```

Add the `clear_spacer` helper near `hide_ghost`:

```c
/* Remove the spacer tag everywhere so no opened gap outlives the ghost. */
static void
clear_spacer (LlmGhostController *self)
{
  if (self->connected_buffer == NULL || self->spacer_tag == NULL)
    return;
  GtkTextIter s, e;
  gtk_text_buffer_get_bounds (self->connected_buffer, &s, &e);
  gtk_text_buffer_remove_tag (self->connected_buffer, self->spacer_tag, &s, &e);
}
```

- [ ] **Step 5: Extend `show_ghost_at_cursor` to render the block**

Replace the existing `show_ghost_at_cursor` body (from the `llm_ghost_overlay_set_text (self->overlay, self->current_ghost);` line through the `gtk_widget_show (...)`/`overlay_visible = TRUE;` end) so it splits the ghost, renders the inline first line, and renders the block with a spacer gap. Concretely, replace everything from:

```c
  llm_ghost_overlay_set_text (self->overlay, self->current_ghost);

  if (!self->overlay_added)
```
down to the end of the function (`self->overlay_visible = TRUE;` then `}`) with:

```c
  /* Split current_ghost into the inline first line and the continuation block. */
  const char *nl = strchr (self->current_ghost, '\n');
  char *first = nl ? g_strndup (self->current_ghost,
                                (gsize) (nl - self->current_ghost))
                   : g_strdup (self->current_ghost);
  const char *rest = nl ? nl + 1 : "";

  llm_ghost_overlay_set_text (self->overlay, first);
  g_free (first);

  /* Inline first line at the cursor (buffer coords; no window conversion). */
  if (!self->overlay_added)
    {
      gtk_text_view_add_child_in_window (self->view,
                                         GTK_WIDGET (self->overlay),
                                         GTK_TEXT_WINDOW_TEXT,
                                         cursor_rect.x, line_y);
      self->overlay_added = TRUE;
    }
  else
    {
      gtk_text_view_move_child (self->view, GTK_WIDGET (self->overlay),
                                cursor_rect.x, line_y);
    }
  gtk_widget_show (GTK_WIDGET (self->overlay));

  /* Continuation block: open a gap below the cursor line and float the block
   * label into it at column 0. */
  clear_spacer (self);
  if (*rest != '\0')
    {
      guint n_cont = _llm_ghost_controller_count_lines (self->current_ghost) - 1;

      if (self->spacer_tag != NULL)
        {
          g_object_set (self->spacer_tag,
                        "pixels-below-lines", (gint) (n_cont * line_h), NULL);
          GtkTextIter ls = cursor_iter, le = cursor_iter;
          gtk_text_iter_set_line_offset (&ls, 0);
          if (!gtk_text_iter_ends_line (&le))
            gtk_text_iter_forward_to_line_end (&le);
          gtk_text_buffer_apply_tag (buffer, self->spacer_tag, &ls, &le);
        }

      /* x of column 0 on the cursor line. */
      GtkTextIter line_start = cursor_iter;
      gtk_text_iter_set_line_offset (&line_start, 0);
      GdkRectangle ls_rect;
      gtk_text_view_get_iter_location (self->view, &line_start, &ls_rect);

      llm_ghost_overlay_set_text (self->overlay_block, rest);
      if (!self->overlay_block_added)
        {
          gtk_text_view_add_child_in_window (self->view,
                                             GTK_WIDGET (self->overlay_block),
                                             GTK_TEXT_WINDOW_TEXT,
                                             ls_rect.x, line_y + line_h);
          self->overlay_block_added = TRUE;
        }
      else
        {
          gtk_text_view_move_child (self->view, GTK_WIDGET (self->overlay_block),
                                    ls_rect.x, line_y + line_h);
        }
      gtk_widget_show (GTK_WIDGET (self->overlay_block));
    }
  else if (self->overlay_block_added)
    {
      gtk_widget_hide (GTK_WIDGET (self->overlay_block));
    }

  self->overlay_visible = TRUE;
}
```

(The `cursor_iter`, `buffer`, `line_y`, `line_h`, and `cursor_rect` locals already exist earlier in the function — this code reuses them.)

- [ ] **Step 6: Reposition the block on scroll, and tear it down on hide**

In `reposition_ghost`, after the existing `gtk_text_view_move_child` for `self->overlay`, add block repositioning. Replace the end of `reposition_ghost` (the single `gtk_text_view_move_child (... self->overlay ...)` call) with:

```c
  gtk_text_view_move_child (self->view, GTK_WIDGET (self->overlay),
                            cursor_rect.x, line_y);

  if (self->overlay_block_added &&
      gtk_widget_get_visible (GTK_WIDGET (self->overlay_block)))
    {
      GtkTextIter line_start = cursor_iter;
      gtk_text_iter_set_line_offset (&line_start, 0);
      GdkRectangle ls_rect;
      gtk_text_view_get_iter_location (self->view, &line_start, &ls_rect);
      gtk_text_view_move_child (self->view, GTK_WIDGET (self->overlay_block),
                                ls_rect.x, line_y + line_h);
    }
```

In `hide_ghost`, after `g_clear_pointer (&self->current_ghost, g_free);`, remove the gap and hide the block. Replace the `hide_ghost` body with:

```c
static void
hide_ghost (LlmGhostController *self)
{
  g_clear_pointer (&self->current_ghost, g_free);
  clear_spacer (self);
  if (self->overlay_block_added)
    gtk_widget_hide (GTK_WIDGET (self->overlay_block));
  if (self->overlay_visible)
    {
      gtk_widget_hide (GTK_WIDGET (self->overlay));
      self->overlay_visible = FALSE;
    }
}
```

In `detach_from_view`, inside the `if (self->view != NULL)` block, alongside the existing `overlay` removal, also remove the block child. After the existing block that does `gtk_container_remove (... self->overlay ...)` / sets `overlay_added = FALSE`, add:

```c
      if (self->overlay_block_added)
        {
          gtk_container_remove (GTK_CONTAINER (self->view),
                                GTK_WIDGET (self->overlay_block));
          self->overlay_block_added = FALSE;
        }
```

- [ ] **Step 7: Build and run the gui suite**

Run: `ninja -C build && meson test -C build --suite gui -v`
Expected: PASS — `multiline-split`, `multiline-cap`, `partial-accept-multi-line`, and all existing controller tests pass.

- [ ] **Step 8: Run the full suite**

Run: `meson test -C build`
Expected: PASS — everything green.

- [ ] **Step 9: Commit**

```bash
git add lib/llmghost-controller.c tests/test-controller.c
git commit -m "feat(controller): multi-line ghost rendering via spacer tag + block overlay"
```

---

### Task 4: OpenAI single-line opt-out

Lets the OpenAI backend stop sending the `\n` stop token when not in single-line mode.

**Files:**
- Modify: `lib/llmghost-openai-backend-internal.h`
- Modify: `lib/llmghost-openai-backend.c`
- Modify: `tests/test-openai-body.c`

- [ ] **Step 1: Write failing tests**

In `tests/test-openai-body.c`, update the existing builder call sites to pass the new trailing `single_line` arg, and add stop-token tests. The two builders currently take `(model, prefix, suffix, max_tokens, temperature, stream)`; they will take `(..., stream, single_line)`. For every existing call, append `, TRUE` (existing tests assume single-line). Then add:

```c
static void
test_completions_stop_single_line (void)
{
  char *on  = _llm_ghost_openai_build_completions_body ("m","p","s",64,0.2, FALSE, TRUE);
  char *off = _llm_ghost_openai_build_completions_body ("m","p","s",64,0.2, FALSE, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stop\""));   /* single-line keeps it */
  g_assert_null    (g_strstr_len (off, -1, "\"stop\""));   /* multi-line drops it */
  g_free (on);
  g_free (off);
}

static void
test_chat_stop_single_line (void)
{
  char *on  = _llm_ghost_openai_build_chat_body ("m","p","s",64,0.2, FALSE, TRUE);
  char *off = _llm_ghost_openai_build_chat_body ("m","p","s",64,0.2, FALSE, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stop\""));
  g_assert_null    (g_strstr_len (off, -1, "\"stop\""));
  g_free (on);
  g_free (off);
}
```

Register in `main`:

```c
  g_test_add_func ("/openai/completions-stop-single-line", test_completions_stop_single_line);
  g_test_add_func ("/openai/chat-stop-single-line",        test_chat_stop_single_line);
```

- [ ] **Step 2: Build to verify failure**

Run: `ninja -C build`
Expected: FAIL — builders called with the wrong arg count.

- [ ] **Step 3: Update the internal header**

In `lib/llmghost-openai-backend-internal.h`, change the two builder prototypes to add a trailing `gboolean single_line`, and add the setter. Replace the two builder declarations with:

```c
char *_llm_ghost_openai_build_completions_body (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature,
                                                gboolean    stream,
                                                gboolean    single_line);

char *_llm_ghost_openai_build_chat_body        (const char *model,
                                                const char *prefix,
                                                const char *suffix,
                                                guint       max_tokens,
                                                double      temperature,
                                                gboolean    stream,
                                                gboolean    single_line);
```

Add before `G_END_DECLS`:

```c
/* When TRUE (default) the request sends "\n" as a stop token (single-line).
 * Set FALSE for multi-line completions. */
void  _llm_ghost_openai_backend_set_single_line (LlmGhostOpenAIBackend *self,
                                                 gboolean               single_line);
```

- [ ] **Step 4: Implement in the backend**

In `lib/llmghost-openai-backend.c`:

(a) Add `gboolean single_line` to both builder signatures (append the param), and guard the stop call. In `_llm_ghost_openai_build_completions_body`, replace `add_stop_newline (b);` with:

```c
  if (single_line)
    add_stop_newline (b);
```
Do the **same** in `_llm_ghost_openai_build_chat_body`.

(b) Add a `gboolean single_line;` field to `struct _LlmGhostOpenAIBackend` (after the existing `gboolean stream;` field), default it in `llm_ghost_openai_backend_init`:

```c
  self->single_line = TRUE;
```

(c) Add the setter near `_llm_ghost_openai_backend_set_stream`:

```c
void
_llm_ghost_openai_backend_set_single_line (LlmGhostOpenAIBackend *self,
                                           gboolean               single_line)
{
  g_return_if_fail (LLM_GHOST_IS_OPENAI_BACKEND (self));
  self->single_line = single_line;
}
```

(d) Update the four builder call sites in `openai_request` to pass `self->single_line` as the final arg (the streaming pair and the non-streaming pair). Each call currently ends with `..., self->temperature, TRUE)` or `..., self->temperature, FALSE)` (the `stream` flag) — append `, self->single_line` to all four.

- [ ] **Step 5: Build and run**

Run: `ninja -C build && meson test -C build openai-body -v`
Expected: PASS — existing tests (with updated args) + the two new stop tests.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-openai-backend.c lib/llmghost-openai-backend-internal.h tests/test-openai-body.c
git commit -m "feat(openai): single_line opt-out drops the newline stop token"
```

---

### Task 5: Ollama + Mistral single-line opt-out

Same pattern for the other two HTTP backends.

**Files:**
- Modify: `lib/llmghost-ollama-backend-internal.h`, `lib/llmghost-ollama-backend.c`, `tests/test-ollama-body.c`
- Modify: `lib/llmghost-mistral-backend-internal.h`, `lib/llmghost-mistral-backend.c`, `tests/test-mistral-body.c`

- [ ] **Step 1: Write failing tests**

In `tests/test-ollama-body.c`, append `, TRUE` to every existing `_llm_ghost_ollama_build_request_body (...)` call, then add:

```c
static void
test_ollama_stop_single_line (void)
{
  const LlmGhostFimTokens *t = llm_ghost_fim_tokens_lookup_builtin ("Qwen");
  char *on  = _llm_ghost_ollama_build_request_body ("m", t, "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_ollama_build_request_body ("m", t, "p", "s", 64, 0.2, FALSE);
  /* single-line: a bare "\n" appears in the stop array; multi-line: it does not.
   * The family sentinel tokens remain in both. */
  g_assert_nonnull (g_strstr_len (on,  -1, "\"\\n\""));
  g_assert_null    (g_strstr_len (off, -1, "\"\\n\""));
  g_free (on);
  g_free (off);
}
```

Register in `main`:

```c
  g_test_add_func ("/ollama-body/stop-single-line", test_ollama_stop_single_line);
```

In `tests/test-mistral-body.c`, append `, TRUE` to every existing `_llm_ghost_mistral_build_fim_body (...)` call, then add:

```c
static void
test_mistral_stop_single_line (void)
{
  char *on  = _llm_ghost_mistral_build_fim_body ("m", "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_mistral_build_fim_body ("m", "p", "s", 64, 0.2, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stop\""));   /* single-line keeps stop */
  g_assert_null    (g_strstr_len (off, -1, "\"stop\""));   /* multi-line omits it */
  g_free (on);
  g_free (off);
}
```

Register in `main`:

```c
  g_test_add_func ("/mistral-body/stop-single-line", test_mistral_stop_single_line);
```

- [ ] **Step 2: Build to verify failure**

Run: `ninja -C build`
Expected: FAIL — builders called with the wrong arg count.

- [ ] **Step 3: Update the Ollama backend**

In `lib/llmghost-ollama-backend-internal.h`, add a trailing `gboolean single_line` to the prototype:

```c
char *_llm_ghost_ollama_build_request_body (const char              *model,
                                            const LlmGhostFimTokens *tokens,
                                            const char              *prefix,
                                            const char              *suffix,
                                            guint                    num_predict,
                                            double                   temperature,
                                            gboolean                 single_line);
```

In `lib/llmghost-ollama-backend.c`:

(a) Add `gboolean single_line` to the `_llm_ghost_ollama_build_request_body` signature (append the param). In the stop-array build, replace:

```c
  json_builder_add_string_value (b, "\n");
  for (gsize i = 0; tokens->stop_tokens != NULL && tokens->stop_tokens[i] != NULL; i++)
    json_builder_add_string_value (b, tokens->stop_tokens[i]);
```
with:
```c
  if (single_line)
    json_builder_add_string_value (b, "\n");
  for (gsize i = 0; tokens->stop_tokens != NULL && tokens->stop_tokens[i] != NULL; i++)
    json_builder_add_string_value (b, tokens->stop_tokens[i]);
```

(b) Add a `gboolean single_line;` field to `struct _LlmGhostOllamaBackend`; default it `TRUE` in the backend's `init`. Add a setter (place near the other backend functions) and declare it in the **public** header `lib/llmghost-ollama-backend.h` (the factory includes that, not the internal one — match how the factory already calls `llm_ghost_ollama_backend_set_fim_tokens`):

In `lib/llmghost-ollama-backend.h`, add:

```c
void llm_ghost_ollama_backend_set_single_line (LlmGhostOllamaBackend *self,
                                               gboolean               single_line);
```

In `lib/llmghost-ollama-backend.c`:

```c
void
llm_ghost_ollama_backend_set_single_line (LlmGhostOllamaBackend *self,
                                          gboolean               single_line)
{
  g_return_if_fail (LLM_GHOST_IS_OLLAMA_BACKEND (self));
  self->single_line = single_line;
}
```

(c) Find the `_llm_ghost_ollama_build_request_body` call site in the request flow and append `, self->single_line`.

- [ ] **Step 4: Update the Mistral backend**

In `lib/llmghost-mistral-backend-internal.h`, add a trailing `gboolean single_line` to the `_llm_ghost_mistral_build_fim_body` prototype.

In `lib/llmghost-mistral-backend.c`:

(a) Add `gboolean single_line` to the builder signature. Replace the stop block:

```c
  json_builder_set_member_name (b, "stop");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, "\n");
  json_builder_end_array (b);
```
with:
```c
  if (single_line)
    {
      json_builder_set_member_name (b, "stop");
      json_builder_begin_array (b);
      json_builder_add_string_value (b, "\n");
      json_builder_end_array (b);
    }
```

(b) Add a `gboolean single_line;` field to `struct _LlmGhostMistralBackend`; default `TRUE` in `init`. Declare a setter in the **public** header `lib/llmghost-mistral-backend.h`:

```c
void llm_ghost_mistral_backend_set_single_line (LlmGhostMistralBackend *self,
                                                gboolean                single_line);
```
and implement it in `lib/llmghost-mistral-backend.c`:

```c
void
llm_ghost_mistral_backend_set_single_line (LlmGhostMistralBackend *self,
                                           gboolean                single_line)
{
  g_return_if_fail (LLM_GHOST_IS_MISTRAL_BACKEND (self));
  self->single_line = single_line;
}
```

(c) Append `, self->single_line` to the `_llm_ghost_mistral_build_fim_body` call site.

- [ ] **Step 5: Build and run**

Run: `ninja -C build && meson test -C build ollama-body mistral-body -v`
Expected: PASS — existing tests (updated args) + the two new stop tests.

- [ ] **Step 6: Commit**

```bash
git add lib/llmghost-ollama-backend.c lib/llmghost-ollama-backend.h lib/llmghost-ollama-backend-internal.h \
        lib/llmghost-mistral-backend.c lib/llmghost-mistral-backend.h lib/llmghost-mistral-backend-internal.h \
        tests/test-ollama-body.c tests/test-mistral-body.c
git commit -m "feat(ollama,mistral): single_line opt-out drops the newline stop token"
```

---

### Task 6: Settings + factory + plugin wiring

Surfaces `max_lines` as a top-level setting: the controller reads it to clamp, the factory reads it to set each backend's single-line mode (`max_lines == 1` → keep the stop token).

**Files:**
- Modify: `lib/llmghost-settings.h`, `lib/llmghost-settings.c`, `tests/test-settings.c`
- Modify: `lib/llmghost-controller.h`, `lib/llmghost-controller.c`
- Modify: `lib/llmghost-backend-factory.c`
- Modify: `plugin/llmghost-plugin.c`

- [ ] **Step 1: Write the failing settings test**

In `tests/test-settings.c`, add a test that a configured `max_lines` is read, mirroring the existing `test_parse_debounce` / `test_parse_debounce_absent` tests (which use the `_llm_ghost_settings_new_from_string` helper and `g_object_unref` to clean up). Add:

```c
static void
test_parse_max_lines (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"max_lines\":5}");
  guint n = 0;
  g_assert_true (llm_ghost_settings_get_max_lines (s, &n));
  g_assert_cmpuint (n, ==, 5);
  g_object_unref (s);
}

static void
test_parse_max_lines_absent (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{}");
  g_assert_false (llm_ghost_settings_get_max_lines (s, NULL));
  g_object_unref (s);
}
```

Register them in `main`, next to the debounce registrations:

```c
  g_test_add_func ("/settings/max-lines",        test_parse_max_lines);
  g_test_add_func ("/settings/max-lines-absent", test_parse_max_lines_absent);
```

- [ ] **Step 2: Build to verify failure**

Run: `ninja -C build`
Expected: FAIL — `llm_ghost_settings_get_max_lines` undeclared.

- [ ] **Step 3: Implement the settings getter**

In `lib/llmghost-settings.h`, after the `llm_ghost_settings_get_debounce_ms` declaration, add:

```c
/* Optional ghost line cap. Returns TRUE and writes *out when the config sets a
 * positive integer "max_lines"; returns FALSE when absent. */
gboolean llm_ghost_settings_get_max_lines (LlmGhostSettings *self,
                                           guint            *out);
```

In `lib/llmghost-settings.c`, add the implementation right after `llm_ghost_settings_get_debounce_ms` (it is the same shape — copy it and swap the key):

```c
gboolean
llm_ghost_settings_get_max_lines (LlmGhostSettings *self, guint *out)
{
  JsonNode *n = json_object_get_member (self->root, "max_lines");
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
    {
      GType t = json_node_get_value_type (n);
      if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE)
        {
          gint64 v = (t == G_TYPE_DOUBLE) ? (gint64) json_node_get_double (n)
                                          : json_node_get_int (n);
          if (v > 0)
            {
              if (out != NULL)
                *out = (guint) v;
              return TRUE;
            }
        }
    }
  return FALSE;
}
```

Add the key to `DEFAULT_SETTINGS_JSON`, after the `"debounce_ms": 80,` line:

```c
  "  \"max_lines\": 8,\n"
```

- [ ] **Step 4: Add the controller setter**

In `lib/llmghost-controller.h`, after `llm_ghost_controller_set_debounce_ms`, add:

```c
void llm_ghost_controller_set_max_lines (LlmGhostController *self,
                                         guint               max_lines);
```

In `lib/llmghost-controller.c`, near `llm_ghost_controller_set_debounce_ms`, add:

```c
void
llm_ghost_controller_set_max_lines (LlmGhostController *self, guint max_lines)
{
  g_return_if_fail (LLM_GHOST_IS_CONTROLLER (self));
  self->max_lines = max_lines > 0 ? max_lines : 1;
}
```

- [ ] **Step 5: Wire the factory (single-line when max_lines == 1)**

In `lib/llmghost-backend-factory.c`, the entry point `llm_ghost_backend_new_from_settings (LlmGhostSettings *settings)` builds the active backend. Read `max_lines` once and apply single-line mode to the built backend. Replace the function body's construction/return so it computes `single_line` and calls the matching setter. Concretely, after the existing `JsonObject *p = llm_ghost_settings_get_backend_params (settings, which);` line, add:

```c
  guint max_lines = 8;
  llm_ghost_settings_get_max_lines (settings, &max_lines);
  gboolean single_line = (max_lines == 1);
```

Then, for each backend branch, set single-line after constructing. Change the `openai`/`mistral`/`ollama`/`generic` dispatch so each built backend gets the flag. Replace:

```c
  if (g_strcmp0 (which, "openai") == 0)
    return build_openai (p);
  if (g_strcmp0 (which, "mistral") == 0)
    return build_mistral (p);
  if (g_strcmp0 (which, "generic") == 0)
    return build_generic (p);
  if (g_strcmp0 (which, "ollama") != 0)
    g_warning ("unknown backend \"%s\"; using ollama", which);
  return build_ollama (p);
```
with:
```c
  LlmGhostBackend *b;
  if (g_strcmp0 (which, "openai") == 0)
    {
      b = build_openai (p);
      _llm_ghost_openai_backend_set_single_line (LLM_GHOST_OPENAI_BACKEND (b),
                                                 single_line);
    }
  else if (g_strcmp0 (which, "mistral") == 0)
    {
      b = build_mistral (p);
      llm_ghost_mistral_backend_set_single_line (LLM_GHOST_MISTRAL_BACKEND (b),
                                                 single_line);
    }
  else if (g_strcmp0 (which, "generic") == 0)
    {
      b = build_generic (p);   /* generic: template owns its stops; flag N/A */
    }
  else
    {
      if (g_strcmp0 (which, "ollama") != 0)
        g_warning ("unknown backend \"%s\"; using ollama", which);
      b = build_ollama (p);
      llm_ghost_ollama_backend_set_single_line (LLM_GHOST_OLLAMA_BACKEND (b),
                                                single_line);
    }
  return b;
```

(`llmghost-openai-backend-internal.h` is already included by the factory from the streaming work; the ollama/mistral setters are in their **public** headers, already included.)

- [ ] **Step 6: Wire the plugin (max_lines → controller)**

In `plugin/llmghost-plugin.c`, in `attach_controller`, after the debounce wiring, add:

```c
  guint mlines;
  if (llm_ghost_settings_get_max_lines (self->settings, &mlines))
    llm_ghost_controller_set_max_lines (ctrl, mlines);
```

- [ ] **Step 7: Build and run the full suite**

Run: `ninja -C build && meson test -C build`
Expected: PASS — settings test + everything else green.

- [ ] **Step 8: Commit**

```bash
git add lib/llmghost-settings.c lib/llmghost-settings.h tests/test-settings.c \
        lib/llmghost-controller.c lib/llmghost-controller.h \
        lib/llmghost-backend-factory.c plugin/llmghost-plugin.c
git commit -m "feat: max_lines setting wires controller clamp + backend single-line mode"
```

---

### Task 7: Documentation

**Files:**
- Modify: `NOTES.md`

- [ ] **Step 1: Mark Phase 4 landed**

In `NOTES.md`, update the "Phase 4 — multi-line ghost rendering (deferred)" section: change the heading to "Phase 4 — multi-line ghost rendering (landed YYYY-MM-DD)" using today's date, and replace the deferral body with a short summary covering: Copilot-style inline-first-line + block-below layout; real push-down via a `pixels-below-lines` spacer tag (no buffer mutation, no undo/modified side effects) plus a second multi-line overlay label (Approach C); the `max_lines` top-level setting (default 8) that clamps in the controller and, when `== 1`, keeps the backends' single-line `\n` stop token; `current_ghost` remains the single source of truth so display == accept; acceptance keys unchanged (Tab/Right/Ctrl+Right now span lines); streaming grows the block live. Also add a one-line note under the "Generic (template) backend" section that template authors should omit a newline stop token to get multi-line output.

- [ ] **Step 2: Run the whole suite once more**

Run: `meson test -C build`
Expected: PASS — every suite green.

- [ ] **Step 3: Commit**

```bash
git add NOTES.md
git commit -m "docs: note multi-line ghost rendering landed"
```

---

## Self-Review

**Spec coverage:**
- Layout (inline first line + block below) → Task 3 (`show_ghost_at_cursor` split). ✓
- Push-down reflow via spacer tag → Task 3 (`spacer_tag`, `pixels-below-lines`, `clear_spacer`). ✓
- Two introspectable overlay labels (Approach C) → Task 2 (multi-line overlay) + Task 3 (`overlay_block`). ✓
- `max_lines` cap, single source of truth → Task 1 (`clamp_ghost_text`), Task 6 (setting). ✓
- Backends drop the newline stop → Tasks 4 (openai), 5 (ollama, mistral); generic documented in Task 7. ✓
- `max_lines: 1` parity (factory keeps the stop) → Task 6 (`single_line = (max_lines == 1)`). ✓
- Streaming grows the block → falls out of Task 1 routing `partial-data` through `clamp_ghost_text` + Task 3 rendering; covered by `test_partial_accept_multi_line`. ✓
- Acceptance unchanged across multi-line → Task 3 tests (`multiline-cap`, `partial-accept-multi-line`). ✓
- Edge cases (last line, hide/detach/dispose teardown of tag + block) → Task 3 (`hide_ghost`, `detach_from_view`, `dispose`). ✓
- Testing strategy (pure clamp/count + gui split/cap/accept + body stop tests + settings) → Tasks 1, 3, 4, 5, 6. ✓

**Placeholder scan:** No TBD/TODO. Every code step shows complete code; every run step shows the command + expected result. The one soft reference — the settings-test constructor helper in Task 6 Step 1 — explicitly instructs matching the existing `debounce_ms` test's pattern in that file (a real, discoverable thing), not a placeholder.

**Type consistency:** `_llm_ghost_controller_clamp_ghost_text(const char*, guint)` and `_llm_ghost_controller_count_lines(const char*)` are declared (Task 1 header), implemented (Task 1), and used (Task 1 call sites, Task 3 renderer) identically. `single_line` is appended as the final `gboolean` arg to all three body builders and threaded through each backend's field/setter/call-sites consistently (Tasks 4, 5). `llm_ghost_settings_get_max_lines(self, guint*)`, `llm_ghost_controller_set_max_lines(self, guint)`, `_llm_ghost_openai_backend_set_single_line`, `llm_ghost_ollama_backend_set_single_line`, `llm_ghost_mistral_backend_set_single_line` are declared and called with matching signatures (Tasks 4–6). `overlay_block` / `overlay_block_added` / `spacer_tag` are introduced together in Task 3 and used consistently across show/reposition/hide/detach/dispose.

**One sequencing note:** Task 1 sets `max_lines = 1` (behavior-preserving); Task 3 flips it to 8 in the same commit that lands the renderer and updates the single-line regression test to its multi-line form — so no commit is left with multi-line data but single-line rendering (display == accept holds throughout).
