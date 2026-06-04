#include <glib.h>
#include "llmghost-settings-internal.h"

static void
test_interpolate_null (void)
{
  char *r = _llm_ghost_settings_interpolate (NULL);
  g_assert_cmpstr (r, ==, "");
  g_free (r);
}

static void
test_interpolate_plain (void)
{
  char *r = _llm_ghost_settings_interpolate ("no vars here");
  g_assert_cmpstr (r, ==, "no vars here");
  g_free (r);
}

static void
test_interpolate_single (void)
{
  g_setenv ("LLMGHOST_TEST_VAR", "value", TRUE);
  char *r = _llm_ghost_settings_interpolate ("a-${LLMGHOST_TEST_VAR}-b");
  g_assert_cmpstr (r, ==, "a-value-b");
  g_free (r);
  g_unsetenv ("LLMGHOST_TEST_VAR");
}

static void
test_interpolate_multiple (void)
{
  g_setenv ("LLMGHOST_TEST_X", "1", TRUE);
  g_setenv ("LLMGHOST_TEST_Y", "2", TRUE);
  char *r = _llm_ghost_settings_interpolate ("${LLMGHOST_TEST_X}.${LLMGHOST_TEST_Y}");
  g_assert_cmpstr (r, ==, "1.2");
  g_free (r);
  g_unsetenv ("LLMGHOST_TEST_X");
  g_unsetenv ("LLMGHOST_TEST_Y");
}

static void
test_interpolate_unset (void)
{
  g_unsetenv ("LLMGHOST_TEST_NOPE");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  char *r = _llm_ghost_settings_interpolate ("x${LLMGHOST_TEST_NOPE}y");
  g_assert_cmpstr (r, ==, "xy");
  g_free (r);
  g_test_assert_expected_messages ();
}

static void
test_interpolate_literal_dollar (void)
{
  /* A '$' not starting a "${...}" is preserved, and an unterminated "${" too. */
  char *a = _llm_ghost_settings_interpolate ("cost is $5");
  g_assert_cmpstr (a, ==, "cost is $5");
  g_free (a);
  char *b = _llm_ghost_settings_interpolate ("broken ${UNCLOSED");
  g_assert_cmpstr (b, ==, "broken ${UNCLOSED");
  g_free (b);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/settings/interpolate/null",           test_interpolate_null);
  g_test_add_func ("/settings/interpolate/plain",          test_interpolate_plain);
  g_test_add_func ("/settings/interpolate/single",         test_interpolate_single);
  g_test_add_func ("/settings/interpolate/multiple",       test_interpolate_multiple);
  g_test_add_func ("/settings/interpolate/unset",          test_interpolate_unset);
  g_test_add_func ("/settings/interpolate/literal-dollar", test_interpolate_literal_dollar);
  return g_test_run ();
}
