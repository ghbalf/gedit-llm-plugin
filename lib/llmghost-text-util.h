#pragma once

/* Internal (NOT installed) shared text helpers for the chat-style backends. */

#include <glib.h>

G_BEGIN_DECLS

/* Trim, unwrap a single leading ``` fence (with optional language tag) and its
 * trailing ```, then truncate at the first newline. NULL-safe; always returns a
 * newly-allocated string (possibly ""). Turns chat-model prose into ghost text. */
char *_llm_ghost_clean_single_line (const char *raw);

G_END_DECLS
