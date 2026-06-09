#pragma once

/* Internal (NOT installed) shared text helpers for the chat-style backends. */

#include <glib.h>

G_BEGIN_DECLS

/* Clean a raw model completion: strip leading/trailing whitespace and remove a
 * surrounding ``` code fence. When @single_line is TRUE, additionally truncate
 * at the first newline (legacy single-line behaviour). When FALSE, all interior
 * lines are preserved (multi-line ghost). */
char *_llm_ghost_clean_text (const char *raw, gboolean single_line);

/* Trim, unwrap a single leading ``` fence (with optional language tag) and its
 * trailing ```, then truncate at the first newline. NULL-safe; always returns a
 * newly-allocated string (possibly ""). Turns chat-model prose into ghost text. */
char *_llm_ghost_clean_single_line (const char *raw);

G_END_DECLS
