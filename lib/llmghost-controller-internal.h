#pragma once

/* Testing-only internal API. NOT installed. Exposes the pure ghost-acceptance
 * boundary helpers for direct unit testing. */

#include <glib.h>

G_BEGIN_DECLS

gsize _llm_ghost_controller_next_char_len (const char *ghost);
gsize _llm_ghost_controller_next_word_len (const char *ghost);


/* Clamp @text to at most @max_lines lines (split on '\n'), then right-trim
 * trailing whitespace and blank lines. Leading whitespace is preserved.
 * Returns a newly-allocated string, or NULL if nothing meaningful remains.
 * @max_lines == 0 is treated as 1. With @max_lines == 1 this reproduces the
 * old single-line "truncate at first newline + right-trim" behavior. */
char *_llm_ghost_controller_clamp_ghost_text (const char *text, guint max_lines);

/* Number of lines in @text (1 + count of '\n'); 0 for NULL/empty. Call this on
 * the result of clamp_ghost_text, not on raw backend text: a trailing newline
 * counts as an extra line, which clamp_ghost_text right-trims away. */
guint _llm_ghost_controller_count_lines (const char *text);

G_END_DECLS
