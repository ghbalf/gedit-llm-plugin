#include <glib.h>
#include "llmghost-secret-store.h"

#define TEST_NAME "llmghost-selftest-key"

/* Is a secret service reachable? Headless CI / no D-Bus session / no
 * gnome-keyring → skip. A reachable service returns NULL + no error for a
 * missing key; an unreachable one sets an error. */
static gboolean
service_available (void)
{
  GError *error = NULL;
  char *v = llm_ghost_secret_lookup ("llmghost-probe-nonexistent", &error);
  g_free (v);
  if (error != NULL)
    {
      g_clear_error (&error);
      return FALSE;
    }
  return TRUE;
}

static void
test_round_trip (void)
{
  if (!service_available ())
    {
      g_test_skip ("no secret service available");
      return;
    }
  GError *error = NULL;

  g_assert_true (llm_ghost_secret_store (TEST_NAME, "sk-secret-123", &error));
  g_assert_no_error (error);

  char *v = llm_ghost_secret_lookup (TEST_NAME, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (v, ==, "sk-secret-123");
  g_free (v);

  g_assert_true (llm_ghost_secret_clear (TEST_NAME, &error));
  g_assert_no_error (error);

  v = llm_ghost_secret_lookup (TEST_NAME, &error);
  g_assert_no_error (error);
  g_assert_null (v);                       /* gone */
  g_free (v);

  g_assert_true (llm_ghost_secret_clear (TEST_NAME, &error));   /* idempotent */
  g_assert_no_error (error);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/secret-store/round-trip", test_round_trip);
  return g_test_run ();
}
