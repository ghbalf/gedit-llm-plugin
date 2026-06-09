# Multi-line Ghost Rendering — Design

**Status:** approved (brainstorming complete)
**Date:** 2026-06-09
**Roadmap:** NOTES.md "Phase 4 — multi-line ghost rendering (deferred)" — now un-deferred (streaming + cloud backends landed).

## Goal

Render real multi-line, Copilot-style block ghost suggestions instead of
truncating completions at the first newline. The first ghost line continues
inline from the cursor; the remaining lines render as a block below, with the
real buffer text **pushed down** so the block never overlaps existing code.

## Background (current behavior)

- `LlmGhostOverlay` is a single-line `GtkLabel` (`single_line_mode = TRUE`);
  `llm_ghost_overlay_set_text` truncates at the first `\n`.
- `LlmGhostController` stores the displayed suggestion in `current_ghost`, which
  is exactly what Tab/Right/Ctrl+Right insert. `sanitize_ghost_text` truncates
  the completion at the first newline and right-trims trailing whitespace, so
  display == accept.
- `show_ghost_at_cursor` anchors the overlay as a `GTK_TEXT_WINDOW_TEXT` child at
  buffer coordinates `(cursor_rect.x, line_y)` using the established GTK3
  quirk: for text-window children the stored (x,y) are treated as **buffer**
  coordinates, so we pass `line_y`/`cursor_rect.x` straight from the validators
  with no `buffer_to_window_coords` step.
- `cursor_safe_for_ghost` only shows a ghost when the rest of the current line
  (cursor → line end) is whitespace.
- Backends force single-line generation by sending `\n` as a stop token
  (OpenAI `completions`, Ollama, Mistral). The generic/template backend leaves
  stops to the user's template.

## Decisions (locked during brainstorming)

1. **Layout:** Copilot-style — first ghost line inline after the cursor on the
   current row; remaining lines as a block on the rows below, each at the left
   text edge (column 0). Real buffer text never moves horizontally.
2. **Collision policy:** **push existing text down** (full reflow) so the whole
   block is always visible, never overlapping real code below.
3. **Line bound:** cap at `max_lines` lines (default 8, configurable). Backends
   stop forcing single-line; the cap is enforced in the controller.
4. **Acceptance keys:** unchanged — Tab = accept all, Right = next char,
   Ctrl+Right = next word, Esc = dismiss. No new keys.
5. **Single source of truth:** `current_ghost` holds the full clamped (≤ N-line)
   string; that is exactly what acceptance inserts, so display == accept.
6. **Streaming:** the block grows live as `partial-data` arrives (no new
   streaming code; we just stop truncating partials at the first newline).

## Rendering approach: spacer tag + two overlay labels (Approach C)

Chosen over (A) full custom Pango draw and (B) a `GtkTextChildAnchor` widget.
Deciding factor: the user cannot manually test over SSH, so gui tests assert on
**widget text** (`gtk_label_get_text`) and positions. Approach C delivers true
push-down while keeping the ghost introspectable in automated tests, and reuses
the existing overlay-anchoring code. (A) leaves nothing to introspect; (B)
mutates the buffer with undo/modified-flag hazards and can't easily produce the
inline-first-line + column-0-block layout.

### The reflow primitive

A dedicated `GtkTextTag` (`ghost_spacer`, created once in the buffer's tag
table) applied to the cursor's line with
`pixels-below-lines = n_continuation_lines × line_h` opens a blank vertical gap
below that line. GTK re-lays-out and the real rows below slide down by exactly
the gap height. Applying/removing a tag changes **no text**: it does not set the
`modified` flag, does not emit `::changed`, and is not recorded by undo. This is
the safe "push down without touching the buffer" mechanism.

### Widgets

The controller owns two `GTK_TEXT_WINDOW_TEXT` overlay children:

- `overlay` — existing single-line `LlmGhostOverlay`, the **inline first line**,
  anchored at `(cursor_rect.x, line_y)`. Unchanged.
- `overlay_block` — a **new multi-line** ghost label (`single_line_mode = FALSE`,
  same greyed-italic markup), lines 2..N, anchored at
  `(line_start_x, line_y + line_h)` (column 0, just below the cursor line). Its
  font is copied from the view so its internal line height matches the view's.

`LlmGhostOverlay` gains an optional multi-line mode (a constructor flag or a
`set_multiline` setter) so the same widget class serves both roles; the
controller holds two instances. `set_text` no longer truncates at `\n` — in
single-line mode the label still collapses to one line via GtkLabel, in
multi-line mode it shows all lines.

### Geometry

All positions use the buffer-coordinate trick (no `buffer_to_window_coords`):

- `line_y`, `line_h` ← `gtk_text_view_get_line_yrange(view, cursor)`.
- `cursor_rect.x` ← `gtk_text_view_get_cursor_locations(view, cursor)` — inline x.
- `line_start_x` ← x from `get_cursor_locations`/`get_iter_location` of an iter at
  the **start** of the cursor line — the block label's column-0 x.
- The spacer gap height and the block label height both derive from
  `n_continuation_lines × line_h`, kept in lockstep (set the tag's
  `pixels-below-lines` from the same line-count the block renders).

### Lifecycle

- `show_ghost_at_cursor` (extended): re-check `cursor_safe_for_ghost`; split
  `current_ghost` into first line + remainder; set `overlay` to the first line;
  if the remainder is non-empty, set `overlay_block`, apply the spacer tag at the
  computed height, position and show the block; else hide the block and ensure no
  spacer tag. Then position/show the inline label as today.
- `reposition_ghost` (scroll): recompute geometry and move **both** labels. The
  spacer tag needs no repositioning — it is part of the text layout and scrolls
  naturally.
- `hide_ghost`: remove the spacer tag, hide both labels, clear `current_ghost`.
- The spacer tag is also removed on buffer-change teardown, view-detach, and
  dispose — anywhere the ghost is torn down — so a stray gap can never outlive
  the ghost.

### Pure helpers (unit-testable without a display)

- `clamp_ghost_text(text, max_lines)` → newly-owned string: keep the first
  `max_lines` lines, right-trim trailing blank lines and trailing whitespace;
  return NULL if nothing meaningful remains. Replaces `sanitize_ghost_text`.
  With `max_lines == 1` it reproduces today's single-line behavior.
- `split_ghost(text)` → first line + remainder (the text after the first `\n`,
  or empty). Drives which label shows what.

The pixel placement stays in `show_ghost_at_cursor`; the *decisions* (how many
lines, what text per label, gap height in line units) are pure and tested
directly.

## Controller orchestration

- New top-level setting `max_lines` (default 8), a sibling of `debounce_ms` in
  `settings.json` and on the controller (e.g. `llm_ghost_controller_set_max_lines`).
- Both result paths (`on_completion_ready` and `on_partial_data`) route text
  through `clamp_ghost_text(text, self->max_lines)` instead of
  `sanitize_ghost_text`, then `show_ghost_at_cursor`.
- `current_ghost` is the clamped multi-line string; acceptance code is unchanged.

## Backend changes (stop forcing single-line)

The `max_lines` *count* is enforced only in the controller; backends merely stop
truncating each completion to one line.

- **OpenAI** `completions` body: drop `"stop": ["\n"]`. Chat body unchanged (no
  `\n` stop). `max_tokens` remains the coarse bound.
- **Ollama:** drop the `\n` injected into the stop list; keep the FIM-family stop
  sentinels (they end the completion, not each line).
- **Mistral** (FIM): drop `\n` from stop if present.
- **Generic/template:** no change in code; document in NOTES that template
  authors should omit a newline stop for multi-line output.

**`max_lines: 1` parity:** when the cap is 1, the controller clamps to a single
line (today's behavior) and the backends keep the `\n` stop for efficiency.
`max_lines` is a single top-level settings field read in **two** independent
places: the **factory** reads it when constructing a backend and sets that
backend's single-line stop behavior (`== 1` → keep the `\n` stop; `> 1` → drop
it); the **controller** reads it to clamp `current_ghost`. No controller→backend
call and no second per-backend knob — both consult the same setting, so nothing
regresses for single-line users.

## Streaming interaction

No new streaming code. `partial-data` already carries accumulated text and
drives a re-render via `on_partial_data`; routing it through `clamp_ghost_text`
(no longer single-line) makes the block grow line-by-line as tokens arrive. The
spacer gap and pushed-down text re-expand on each emission. The in-flight gate is
unchanged.

## Acceptance semantics

`current_ghost` is the full clamped multi-line string, so existing code works:

- **Tab** inserts all of `current_ghost` (multi-line); ghost cleared, spacer tag
  removed, inserted newlines become real text.
- **Right** / **Ctrl+Right** accept a prefix; the remainder re-renders via
  `show_ghost_at_cursor`, which re-derives geometry from the moved cursor. A
  newline counts as one char (next-char) or as whitespace (next-word).
- **Esc** dismisses (removes spacer tag, hides both labels).

## Edge cases

- **Cursor on the last buffer line** (common authoring case): nothing below to
  push; the gap extends the document bottom and the block sits in it.
- **Wrapped cursor line:** `pixels-below-lines` applies after the paragraph's
  last display row; the block sits below the wrap.
- **Cursor move / buffer change mid-ghost:** existing teardown runs and now also
  removes the spacer tag; the `inserting_acceptance` guard still prevents
  self-triggered restarts.
- **`cursor_safe_for_ghost` unchanged:** still gates on the rest of the cursor
  line being whitespace (governs the inline first line). Collisions with rows
  below are handled by the reflow itself, so no extra gating.
- **Mid-stream failure:** unchanged — `request_finish` returns the error, ghost
  clears, tag removed.

## Testing strategy

All automated — pure `unit` tests + headless `gui` (xvfb) tests. No manual GUI.

**Unit:**
- `clamp_ghost_text`: caps at N lines; trims trailing blank lines; passes through
  ≤ N input; empty → NULL; `max_lines: 1` == single-line.
- `split_ghost`: first/remainder split incl. no-newline and trailing-newline.
- Backend body builders: `\n` stop **absent** in multi-line mode and **present**
  in single-line mode — extend `test-openai-body`, plus Ollama and Mistral body
  tests.

**GUI (`controller`):**
- Multi-line completion: `current_ghost` holds the full block; inline label text
  == line 1; `overlay_block` text == lines 2..N (via `gtk_label_get_text`).
- Spacer tag applied to the cursor line with `pixels-below-lines == n_cont ×
  line_h`; removed on hide / accept / Esc.
- Tab inserts the full multi-line text (assert `buffer_text`); Right/Ctrl+Right
  accept a prefix and the block re-renders.
- Cap regression: an (N+2)-line completion shows and accepts exactly N lines.
- Streaming: multi-line partials grow the block; gated-idle still suppresses.

The mock backend already emits arbitrary text (including newlines) via existing
helpers.

## Files (anticipated)

- `lib/llmghost-overlay.{c,h}` — multi-line mode; `set_text` stops truncating.
- `lib/llmghost-controller.c` (+ `-internal.h`) — `overlay_block`, spacer tag,
  `clamp_ghost_text`/`split_ghost`, `max_lines`, extended show/reposition/hide.
- `lib/llmghost-openai-backend.c`, `llmghost-ollama-backend.c`,
  `llmghost-mistral-backend.c` — drop the `\n` stop (gated by single-line mode).
- `lib/llmghost-backend-factory.c` (and wherever `debounce_ms` is read) —
  read top-level `max_lines`; pass single-line intent (`== 1`) to each backend
  at construction; surface `max_lines` to the controller alongside `debounce_ms`.
- `tests/test-controller.c`, `test-openai-body.c`, Ollama/Mistral body tests,
  new pure tests for `clamp_ghost_text`/`split_ghost`.
- `NOTES.md` — mark Phase 4 landed; document `max_lines` and the template
  newline-stop guidance.

## Out of scope / deferred

- Mid-line ghost (showing suggestions when non-whitespace follows the cursor) —
  remains Phase 7.
- Per-line "accept line" keybinding — YAGNI; revisit if requested.
- Variable per-line heights: the gap uses `n_cont × line_h` (the cursor line's
  height) uniformly; pathological mixed-height lines may misalign slightly —
  acceptable for v1.
