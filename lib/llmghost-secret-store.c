#define G_LOG_DOMAIN "llmghost-secret"

/* Acknowledge libsecret's "API subject to change" guard before the include. */
#define SECRET_API_SUBJECT_TO_CHANGE
#include "llmghost-secret-store.h"

#include <libsecret/secret.h>

/* One schema for all llmghost secrets, keyed by a single "name" attribute. */
static const SecretSchema *
llm_ghost_secret_schema (void)
{
  /* Suppress -Wmissing-field-initializers: the SecretSchema reserved fields
   * are private and intentionally zero-filled by the aggregate initializer. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  static const SecretSchema schema = {
    "de.mickautsch.llmghost", SECRET_SCHEMA_NONE,
    {
      { "name", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { NULL, 0 },
    }
  };
#pragma GCC diagnostic pop
  return &schema;
}

char *
llm_ghost_secret_lookup (const char *name, GError **error)
{
  g_return_val_if_fail (name != NULL, NULL);
  char *secret = secret_password_lookup_sync (llm_ghost_secret_schema (),
                                              NULL, error,
                                              "name", name, NULL);
  if (secret == NULL)
    return NULL;                          /* not found, or *error set */
  char *out = g_strdup (secret);          /* normalize ownership to g_free */
  secret_password_free (secret);
  return out;
}

gboolean
llm_ghost_secret_store (const char *name, const char *value, GError **error)
{
  g_return_val_if_fail (name != NULL && value != NULL, FALSE);
  char *label = g_strdup_printf ("llmghost: %s", name);
  gboolean ok = secret_password_store_sync (llm_ghost_secret_schema (),
                                           SECRET_COLLECTION_DEFAULT, label, value,
                                           NULL, error,
                                           "name", name, NULL);
  g_free (label);
  return ok;
}

gboolean
llm_ghost_secret_clear (const char *name, GError **error)
{
  g_return_val_if_fail (name != NULL, FALSE);
  /* clear_sync returns FALSE with no error when nothing matched — treat that
   * "nothing to clear" as success; only a set GError is a real failure. */
  GError *local = NULL;
  secret_password_clear_sync (llm_ghost_secret_schema (), NULL, &local,
                              "name", name, NULL);
  if (local != NULL)
    {
      g_propagate_error (error, local);
      return FALSE;
    }
  return TRUE;
}
