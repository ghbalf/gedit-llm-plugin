#include <glib.h>
#include <string.h>
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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ghost-accept/next-char-len", test_next_char_len);
  g_test_add_func ("/ghost-accept/next-word-len", test_next_word_len);
  g_test_add_func ("/ghost-accept/clamp-single-line",   test_clamp_single_line);
  g_test_add_func ("/ghost-accept/clamp-multi-line",    test_clamp_multi_line);
  g_test_add_func ("/ghost-accept/clamp-trailing-blank", test_clamp_trailing_blank_lines);
  g_test_add_func ("/ghost-accept/clamp-empty-null",    test_clamp_empty_is_null);
  g_test_add_func ("/ghost-accept/count-lines",         test_count_lines);
  return g_test_run ();
}
