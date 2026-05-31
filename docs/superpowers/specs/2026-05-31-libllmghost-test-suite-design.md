# libllmghost test suite — design

**Date:** 2026-05-31
**Status:** approved design, pre-implementation
**Scope:** First automated test suite for `libllmghost`, landed *before* the
Phase 5/6 backend work so those backends arrive with a regression net.

## Goal

Stand up a `g_test`-based suite, wired into `meson test`, covering the
library bottom-up: pure value logic, the request-body builder, the async
backend contract, and the GTK controller's observable behaviour (headless
under Xvfb). The controller tests include *sanity* coordinate checks that
guard the Phase-3 buffer-vs-window coordinate regression without pinning to
exact pixels.

This is the **"everything incl. controller"** scope at **"behavior + sanity
coords"** depth (the two decisions taken during brainstorming).

## Non-goals

- No network/live-Ollama tests — the Ollama backend is exercised at the
  pure-function level (`build_request_body`) only.
- No exact-pixel coordinate assertions (deliberately rejected as
  machine/font/theme-fragile).
- No tests for the gedit plugin layer (`plugin/`) — it is thin libpeas glue
  and depends on a running gedit; out of scope for unit testing.
- No multi-line ghost (Phase 4) behaviour — not implemented yet.

## Approach

GLib `g_test` (cases via `g_test_add_func`, `g_assert_*` macros) integrated
with meson's native `test()`. A testing-only internal header exposes the one
`static` pure function we need to test directly. A purpose-built instrumented
mock backend (in `tests/`) makes debounce-coalescing and in-flight
cancellation observable — things the production `LlmGhostFakeBackend` cannot
show because it completes synchronously and ignores its `GCancellable`.

## Components

### Meson layout

Four test executables in `tests/`, in two suites:

| Executable         | Suite  | Needs display |
|--------------------|--------|---------------|
| `test-fim-tokens`  | `unit` | no            |
| `test-ollama-body` | `unit` | no            |
| `test-fake-backend`| `unit` | no            |
| `test-controller`  | `gui`  | yes (Xvfb)    |

Each is an `executable()` linked against `llmghost_dep`, registered with
`test(name, exe, suite: ...)`.

**Xvfb wiring** (top-level `meson.build`):

```meson
xvfb_run = find_program('xvfb-run', required: false)
if xvfb_run.found()
  add_test_setup('xvfb', exe_wrapper: [xvfb_run, '-a'], is_default: true)
endif
```

- `is_default: true` → a plain `meson test -C build` "just works" headless;
  pure-logic tests run under a throwaway X server too (harmless).
- The `gui` test and its `test()` registration are guarded on
  `xvfb_run.found()`, so the suite degrades gracefully where `xvfb-run` is
  absent.
- `meson test -C build --suite unit` runs the display-free tests only.

Confirmed available on the dev box: `xvfb 2:21.1.12-1ubuntu1.5`,
`/usr/bin/xvfb-run`.

### Internal header for the pure builder

New non-installed header `lib/llmghost-ollama-backend-internal.h`:

```c
char *_llm_ghost_ollama_build_request_body (const char              *model,
                                            const LlmGhostFimTokens *tokens,
                                            const char              *prefix,
                                            const char              *suffix,
                                            guint                    num_predict,
                                            double                   temperature);
```

In `llmghost-ollama-backend.c`, the existing `static build_request_body` is
renamed to `_llm_ghost_ollama_build_request_body` and de-`static`'d; the file
includes the new header. No logic change. The header is **not** added to
`llmghost_headers` (kept out of the installed/public set).

### `tests/mock-backend.{h,c}` — instrumented test double

A `GObject` implementing `LlmGhostBackend`, **deferred-by-default** so tests
drive completion timing:

- `LlmGhostBackend *mock_backend_new (void);`
- `void  mock_backend_set_response   (LlmGhostBackend *self, const char *text);`
- `guint mock_backend_request_count  (LlmGhostBackend *self);` — requests that reached the backend
- `guint mock_backend_cancel_count   (LlmGhostBackend *self);` — requests whose `GCancellable` fired
- `void  mock_backend_complete_pending(LlmGhostBackend *self);` — flush stored `GTask`s with the canned response

Cancellation is observed via `g_cancellable_connect` on the request's
cancellable; on fire it increments the counter and the task returns
`G_IO_ERROR_CANCELLED`. Pending non-cancelled tasks complete with the canned
string on `mock_backend_complete_pending`.

## Test cases

### `test-fim-tokens` (pure)

- `new()` copies all fields (deep).
- `copy()` is independent: free the original, the copy stays valid.
- `free(NULL)` is safe.
- Builtins (qwen/starcoder/deepseek) are non-NULL, have expected `name`s and
  distinct `prefix_tok`s.
- `builtins()` is a NULL-terminated array of exactly 3, in display order.
- `lookup_builtin` is case-insensitive (`"qwen"`/`"QWEN"`/`"Qwen"` → qwen);
  unknown name → NULL.
- `stop_tokens` is NULL-terminated.

### `test-ollama-body` (pure)

Feed known inputs to `_llm_ghost_ollama_build_request_body`, parse the result
with json-glib, assert:

- `model` echoes input.
- `raw == true`, `stream == false`.
- `prompt == prefix_tok + prefix + suffix_tok + suffix + middle_tok`
  (using qwen tokens).
- `options.num_predict` and `options.temperature` match inputs.
- `options.stop[0] == "\n"`, followed by the family stop tokens in order.
- NULL `prefix`/`suffix` degrade to empty strings (no crash, tokens still
  concatenated).

### `test-fake-backend` (async contract)

Drive the real `LlmGhostFakeBackend` through the public interface
(`llm_ghost_backend_request` + nested `GMainLoop` + `request_finish`):

- Canned response round-trips.
- NULL canned yields the `"// hello, ghost!"` default.

Validates the GTask async plumbing every backend depends on.

### `test-controller` (gui — behavior + sanity coords)

**Harness:** a realized `GtkWindow` + `GtkTextView`, a mock backend, a
controller with a small debounce (`llm_ghost_controller_set_debounce_ms`),
and main-loop pump helpers.

**Ghost-visibility probe:** `find_ghost_overlay(view)` walks
`gtk_container_get_children(GTK_CONTAINER(view))` for the `LlmGhostOverlay`
child. "Ghost shown" == that child exists and `gtk_widget_get_visible()` is
TRUE. (Confirmed reliable: the controller toggles the overlay via
`gtk_widget_show`/`gtk_widget_hide` at controller.c:502/533/557, and the
overlay sets `no_show_all = TRUE`.) No controller-internal accessor needed.

Behavioural cases:

- **Debounce coalesces:** insert several characters synchronously (the
  debounce `g_timeout` hasn't fired because the loop hasn't run), then pump
  past the debounce → `mock_backend_request_count == 1`.
- **Cancellation on new input:** issue a deferred request, then type again
  before completing → `mock_backend_cancel_count >= 1`; after
  `mock_backend_complete_pending`, only the latest completion renders.
- **Tab accepts:** mock returns `"abc"`, complete pending, synthesize a
  `GDK_KEY_Tab` key-press via `gtk_widget_event(view, ev)` → buffer contains
  `"abc"` at the cursor, ghost no longer visible.
- **Esc dismisses:** synthesize `GDK_KEY_Escape` → buffer unchanged, ghost no
  longer visible.
- **Mid-line suppression:** cursor placed before non-whitespace on the line
  → no visible ghost (`cursor_safe_for_ghost` path).

Sanity-coords case (Phase-3 regression guard):

- Insert enough text to scroll the view, move the cursor to a visible line,
  trigger and render a ghost.
- Compute the cursor line's window-y: `gtk_text_view_get_iter_location` →
  `gtk_text_view_buffer_to_window_coords`.
- Read the overlay child's allocated `y` and assert it agrees with the cursor
  window-y within ~one line height.
- Rationale: a window-vs-buffer coordinate mixup (the exact Phase-3 bug)
  throws this off by the scroll offset; font/theme variation stays within
  tolerance.

## Key GTK testing details (known sharp edges)

- **Key synthesis (GTK 3):** build with `gdk_event_new(GDK_KEY_PRESS)`, set
  `event->window` to the view's `GTK_TEXT_WINDOW_TEXT` (or widget) `GdkWindow`,
  set `keyval` (`GDK_KEY_Tab` / `GDK_KEY_Escape`), dispatch via
  `gtk_widget_event`. Requires the view realized — guaranteed under Xvfb.
- **Deterministic debounce:** because the debounce is a `g_timeout` serviced by
  the main loop, edits applied without iterating the loop cannot fire the
  timer — so "rapid succession" is modelled by mutating the buffer N times,
  then pumping once past the debounce. No real-time sleeps needed for the
  coalescing assertion.
- **Layout validation:** `get_iter_location` can return stale values before
  layout; pump the loop / use `get_line_yrange` to force validation before
  reading coordinates (mirrors the load-bearing call noted in NOTES.md
  Phase 3).

## Files touched

| File | Change |
|------|--------|
| `meson.build` (top) | add `xvfb-run` detection + `add_test_setup` |
| `tests/meson.build` | register four test executables + `mock-backend` sources |
| `lib/llmghost-ollama-backend-internal.h` | **new** — declare the pure builder |
| `lib/llmghost-ollama-backend.c` | rename/de-`static` builder, include internal header |
| `tests/mock-backend.h` / `tests/mock-backend.c` | **new** — instrumented backend |
| `tests/test-fim-tokens.c` | **new** |
| `tests/test-ollama-body.c` | **new** |
| `tests/test-fake-backend.c` | **new** |
| `tests/test-controller.c` | **new** |

## Verification

- `meson test -C build` → all suites green (gui under default Xvfb wrapper).
- `meson test -C build --suite unit` → display-free subset green.
- Existing `meson compile -C build` still succeeds (the demo target is
  unaffected).
```
1. Pure-logic + builder tests → verify: `meson test --suite unit` green
2. Async fake-backend test    → verify: included in `--suite unit`, green
3. Mock backend + controller  → verify: `meson test --suite gui` green under Xvfb
4. Full run                   → verify: `meson test` green end-to-end
```
