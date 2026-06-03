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
  /* multi-byte word char: "café" — é is U+00E9 (2 bytes), whole word is 5 bytes */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("caf\xC3\xA9"),  ==, 5);
  /* multi-byte leading space: U+00A0 NO-BREAK SPACE (2 bytes) + "x" word run */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("\xC2\xA0x"),    ==, 3);
  /* all-whitespace ghost: leading-WS loop drains to NUL, returns full span */
  g_assert_cmpuint (_llm_ghost_controller_next_word_len ("  "),           ==, 2);
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
