#pragma once

/* Testing-only internal API. NOT installed. Exposes the pure ghost-acceptance
 * boundary helpers for direct unit testing. */

#include <glib.h>

G_BEGIN_DECLS

gsize _llm_ghost_controller_next_char_len (const char *ghost);
gsize _llm_ghost_controller_next_word_len (const char *ghost);

G_END_DECLS
