#include <glib.h>
#include "llmghost-text-util.h"

static void
check_clean (const char *raw, const char *expect)
{
  char *got = _llm_ghost_clean_single_line (raw);
  g_assert_cmpstr (got, ==, expect);
  g_free (got);
}

static void
test_clean_single_line (void)
{
  check_clean ("abc", "abc");
  check_clean ("  abc  ", "abc");
  check_clean (NULL, "");
  check_clean ("", "");
  check_clean ("foo()\nbar()", "foo()");                    /* truncate at newline */
  check_clean ("```\nfoo()\n```", "foo()");                 /* bare fence */
  check_clean ("```c\nfoo()\n```", "foo()");                /* lang-tagged fence */
  check_clean ("```python\nx = 1\ny = 2\n```", "x = 1");    /* fence + truncate */
}

static void
test_clean_text_multiline (void)
{
  /* single_line = FALSE keeps every line, still strips outer ws + unfences */
  char *a = _llm_ghost_clean_text ("foo()\nbar()", FALSE);
  g_assert_cmpstr (a, ==, "foo()\nbar()");
  g_free (a);

  char *b = _llm_ghost_clean_text ("```python\nx = 1\ny = 2\n```", FALSE);
  g_assert_cmpstr (b, ==, "x = 1\ny = 2");
  g_free (b);

  char *c = _llm_ghost_clean_text ("  foo\nbar  ", FALSE);
  g_assert_cmpstr (c, ==, "foo\nbar");
  g_free (c);

  /* single_line = TRUE still truncates (parity with clean_single_line) */
  char *d = _llm_ghost_clean_text ("foo()\nbar()", TRUE);
  g_assert_cmpstr (d, ==, "foo()");
  g_free (d);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/text-util/clean-single-line", test_clean_single_line);
  g_test_add_func ("/text-util/clean-text-multiline", test_clean_text_multiline);
  return g_test_run ();
}
