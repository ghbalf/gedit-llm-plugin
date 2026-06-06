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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/text-util/clean-single-line", test_clean_single_line);
  return g_test_run ();
}
