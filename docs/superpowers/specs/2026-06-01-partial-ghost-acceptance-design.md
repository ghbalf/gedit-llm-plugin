# Partial ghost-text acceptance — design

**Date:** 2026-06-01
**Status:** approved design, pre-implementation
**Scope:** Add per-character and per-word acceptance of ghost completions to
`LlmGhostController`, alongside the existing Tab=accept-all and Esc=dismiss.

## Goal

When a ghost suggestion is visible:

- **Right** accepts the next single character of the ghost.
- **Ctrl+Right** accepts the next "word" of the ghost.
- **Tab** still accepts the whole suggestion; **Esc** still dismisses.

After a partial accept, the remainder stays visible as ghost text (so Tab can
still take the rest, or further Right/Ctrl+Right can keep nibbling). Accepting
the final chunk clears the overlay.

This is the Supermaven/Copilot partial-accept UX.

## Non-goals

- No re-fetching from the backend on partial accept — we slice the
  already-rendered suggestion (no latency, no flicker).
- No change to multi-line ghost (still Phase-4 deferred; suggestions are
  single-line after the newline-truncation in `on_completion_ready`).
- No change to the backend, overlay, or gedit plugin layers.
- No configurability of the keybindings or word definition (YAGNI; revisit if
  asked).

## Word-boundary definition (the key decision)

"Accept next word" = **leading whitespace + the following run of word
characters**, stopping before the next punctuation or space. A "word
character" is a Unicode alphanumeric or `_`. When the first non-space
character is punctuation, accept exactly that one character. This guarantees
forward progress on any non-empty ghost.

Worked example — ghost `␣␣foo_bar(x)`:

| Press        | Accepts     | Remainder  |
|--------------|-------------|------------|
| Ctrl+Right   | `␣␣foo_bar` | `(x)`      |
| Ctrl+Right   | `(`         | `x)`       |
| Ctrl+Right   | `x`         | `)`        |
| Ctrl+Right   | `)`         | (empty)    |

## Behavior details

- **Modifiers:** plain Right (no modifiers) → char accept; Ctrl-only Right →
  word accept. If Shift or any other modifier is held, propagate to GTK so
  normal cursor movement / selection still works. Modifier comparison uses
  `event->state & gtk_accelerator_get_default_mod_mask ()` so Caps/Num lock
  don't interfere.
- **Keypad:** handle `GDK_KEY_KP_Right` as well as `GDK_KEY_Right`, mirroring
  the existing `GDK_KEY_KP_Tab` handling.
- **Only when a ghost is visible:** `on_view_key_press` already returns
  `GDK_EVENT_PROPAGATE` early when `!overlay_visible`, so Right behaves as a
  normal cursor move when there's no suggestion.
- **Re-render position:** after inserting the accepted slice the cursor
  advances; `show_ghost_at_cursor` re-checks `cursor_safe_for_ghost` and
  repositions the overlay to the new cursor location, so the remainder renders
  correctly.

## Components

All changes are in `lib/llmghost-controller.c` plus a new internal header.

### `accept_ghost_prefix (self, n_bytes)` — new static helper

Accepts the first `n_bytes` of `current_ghost`:

```c
static void
accept_ghost_prefix (LlmGhostController *self, gsize n_bytes)
{
  if (self->view == NULL || self->current_ghost == NULL || n_bytes == 0)
    return;
  n_bytes = MIN (n_bytes, strlen (self->current_ghost));

  char *accepted = g_strndup (self->current_ghost, n_bytes);
  char *rest     = g_strdup (self->current_ghost + n_bytes);

  GtkTextBuffer *buffer = gtk_text_view_get_buffer (self->view);
  self->inserting_acceptance = TRUE;             /* suppress restart_request */
  gtk_text_buffer_insert_at_cursor (buffer, accepted, -1);
  self->inserting_acceptance = FALSE;
  g_free (accepted);

  g_clear_pointer (&self->current_ghost, g_free);
  if (*rest != '\0')
    { self->current_ghost = rest; show_ghost_at_cursor (self); }
  else
    { g_free (rest); hide_ghost (self); }
}
```

The `inserting_acceptance` guard (already used by the old `accept_ghost`)
prevents the buffer insert from triggering `restart_request` (which would
cancel + hide + re-fetch).

### `accept_ghost (self)` — refactored to delegate

```c
static void
accept_ghost (LlmGhostController *self)
{
  if (self->current_ghost != NULL)
    accept_ghost_prefix (self, strlen (self->current_ghost));
}
```

Full-accept and partial-accept now share one code path. Observable behavior is
unchanged (buffer gets the whole suggestion, overlay hides), so the existing
Tab-accept gui test guards the refactor.

### Two pure boundary functions (exposed for testing)

```c
gsize
_llm_ghost_controller_next_char_len (const char *ghost)
{
  if (ghost == NULL || *ghost == '\0') return 0;
  return (gsize) (g_utf8_next_char (ghost) - ghost);
}

gsize
_llm_ghost_controller_next_word_len (const char *ghost)
{
  if (ghost == NULL || *ghost == '\0') return 0;
  const char *p = ghost;
  while (*p && g_unichar_isspace (g_utf8_get_char (p)))      /* leading whitespace */
    p = g_utf8_next_char (p);
  if (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isalnum (c) || c == '_')                 /* word run */
        while (*p && (g_unichar_isalnum (g_utf8_get_char (p)) ||
                      g_utf8_get_char (p) == '_'))
          p = g_utf8_next_char (p);
      else                                                   /* one punctuation char */
        p = g_utf8_next_char (p);
    }
  return (gsize) (p - ghost);
}
```

UTF-8 correct; both return a positive span for non-empty input.

### `on_view_key_press` — new cases

Inside the existing `switch (event->keyval)` (which is already gated on
`overlay_visible`):

```c
guint mods = event->state & gtk_accelerator_get_default_mod_mask ();
...
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

`mods` is computed once near the top of the handler.

### `lib/llmghost-controller-internal.h` — new, non-installed

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

`llmghost-controller.c` includes it; the two functions are defined non-`static`.
The header is NOT added to `llmghost_headers` in `lib/meson.build`.

## Testing

TDD — tests written before the implementation.

### `tests/test-ghost-accept.c` (new, suite `unit`, no display)

Direct tests of the boundary functions via the internal header:

- `next_char_len`: `"abc"`→1; `""`→0; `NULL`→0; `"éx"`→2 (the `é` is 2 bytes).
- `next_word_len`:
  - `"  foo_bar(x)"` → `strlen("  foo_bar")` (= 9)
  - `"(x)"` → 1, `"x)"` → 1, `")"` → 1
  - `"foo bar"` → 3
  - `" bar"` → `strlen(" bar")` (= 4)
  - `"a1_b="` → `strlen("a1_b")` (= 4)
  - `""` → 0, `NULL` → 0

Register a `test-ghost-accept` executable (built from `test-ghost-accept.c`,
depending on `[llmghost_dep]`) under `suite: 'unit'`.

### `tests/test-controller.c` (extend, suite `gui`)

Add `send_key_mod (Fixture *f, guint keyval, guint state)`; the existing
`send_key` delegates to it with `state == 0`. New cases:

- `/controller/right-accepts-char`: type `f`, complete with mock response
  `"abc"`, ghost visible. Right → buffer `"fa"`, ghost still visible. Right →
  `"fab"`. Right → `"fabc"`, ghost no longer visible.
- `/controller/ctrl-right-accepts-word`: mock response `"foo bar"`, type `x`,
  complete, ghost visible. Ctrl+Right → buffer `"xfoo"`, ghost visible
  (`" bar"` remains). Ctrl+Right → `"xfoo bar"`, ghost not visible.
- `/controller/ctrl-right-punctuation`: mock response `"ab(c"`, type `x`,
  complete. Ctrl+Right → `"xab"` (ghost `"(c"`); Ctrl+Right → `"xab("` (ghost
  `"c"`); Ctrl+Right → `"xab(c"`, ghost not visible.

## Files touched

| File | Change |
|------|--------|
| `lib/llmghost-controller.c` | add `accept_ghost_prefix` + 2 boundary fns; refactor `accept_ghost`; add Right/KP_Right cases + `mods`; include internal header |
| `lib/llmghost-controller-internal.h` | **new** — declare the 2 boundary fns |
| `tests/test-ghost-accept.c` | **new** — unit tests for the boundary fns |
| `tests/test-controller.c` | add `send_key_mod` + 3 gui tests |
| `tests/meson.build` | register `test-ghost-accept` (unit) |

## Verification

- `meson test -C build --suite unit` → green (incl. new `ghost-accept`).
- `meson test -C build controller` → green (incl. 3 new cases + unchanged
  Tab/Esc).
- `meson test -C build` → all green; `meson compile -C build` clean.
```
1. Boundary fns + internal header + unit tests → verify: `meson test --suite unit` green
2. accept_ghost_prefix + accept_ghost refactor   → verify: existing controller tests still green
3. Right/Ctrl+Right key handling + gui tests      → verify: `meson test controller` green
4. Full run                                       → verify: `meson test` green end-to-end
```
