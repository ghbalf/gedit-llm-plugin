#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "llmghost-settings.h"
#include "llmghost-settings-internal.h"
#include "llmghost-backend-factory.h"
#include "llmghost-ollama-backend.h"
#include "llmghost-openai-backend.h"
#include "llmghost-mistral-backend.h"
#include "llmghost-generic-backend.h"

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

static char *
fake_secret_lookup (const char *name)
{
  if (g_strcmp0 (name, "openai") == 0)
    return g_strdup ("sk-from-keyring");
  return NULL;                              /* unknown → unavailable */
}

static void
test_interpolate_secret_found (void)
{
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  char *r = _llm_ghost_settings_interpolate ("Bearer ${secret:openai}!");
  g_assert_cmpstr (r, ==, "Bearer sk-from-keyring!");
  g_free (r);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
}

static void
test_interpolate_secret_missing (void)
{
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not available*");
  char *r = _llm_ghost_settings_interpolate ("k=${secret:nope};");
  g_test_assert_expected_messages ();
  g_assert_cmpstr (r, ==, "k=;");
  g_free (r);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
}

static void
test_interpolate_secret_and_env_coexist (void)
{
  g_setenv ("LLMGHOST_TEST_E", "ENV", TRUE);
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  char *r = _llm_ghost_settings_interpolate ("${secret:openai}/${LLMGHOST_TEST_E}");
  g_assert_cmpstr (r, ==, "sk-from-keyring/ENV");
  g_free (r);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
  g_unsetenv ("LLMGHOST_TEST_E");
}

static void
test_interpolate_secret_in_array (void)
{
  /* ${secret:} nested inside a backends params array+object (generic shape). */
  _llm_ghost_settings_set_secret_lookup_for_testing (fake_secret_lookup);
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backends\":{\"generic\":{\"request_template\":"
      "{\"messages\":[{\"content\":\"${secret:openai}\"}]}}}}");
  JsonObject *p = llm_ghost_settings_get_backend_params (s, "generic");
  JsonObject *t = json_object_get_object_member (p, "request_template");
  JsonArray  *m = json_object_get_array_member (t, "messages");
  JsonObject *m0 = json_array_get_object_element (m, 0);
  g_assert_cmpstr (json_object_get_string_member (m0, "content"), ==, "sk-from-keyring");
  g_object_unref (s);
  _llm_ghost_settings_set_secret_lookup_for_testing (NULL);
}

static void
test_parse_active_backend (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"mistral\",\"backends\":{}}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "mistral");
  g_object_unref (s);
}

static void
test_parse_active_backend_default (void)
{
  /* Missing "backend" → "ollama". */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"backends\":{}}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "ollama");
  g_object_unref (s);
}

static void
test_parse_unknown_backend_passthrough (void)
{
  /* An unrecognised but well-formed string is returned verbatim; the factory,
   * not the accessor, maps unknown → ollama. */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"frobnicate\"}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "frobnicate");
  g_object_unref (s);
}

static void
test_parse_debounce (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"debounce_ms\":120}");
  guint ms = 0;
  g_assert_true (llm_ghost_settings_get_debounce_ms (s, &ms));
  g_assert_cmpuint (ms, ==, 120);
  g_object_unref (s);
}

static void
test_parse_debounce_absent (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{}");
  guint ms = 999;
  g_assert_false (llm_ghost_settings_get_debounce_ms (s, &ms));
  g_object_unref (s);
}

static void
test_parse_max_lines (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"max_lines\":5}");
  guint n = 0;
  g_assert_true (llm_ghost_settings_get_max_lines (s, &n));
  g_assert_cmpuint (n, ==, 5);
  g_object_unref (s);
}

static void
test_parse_max_lines_absent (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{}");
  g_assert_false (llm_ghost_settings_get_max_lines (s, NULL));
  g_object_unref (s);
}

static void
test_parse_backend_params_interpolated (void)
{
  g_setenv ("LLMGHOST_TEST_KEY", "sk-xyz", TRUE);
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backends\":{\"openai\":{\"model\":\"gpt\",\"api_key\":\"${LLMGHOST_TEST_KEY}\"}}}");
  JsonObject *p = llm_ghost_settings_get_backend_params (s, "openai");
  g_assert_nonnull (p);
  g_assert_cmpstr (json_object_get_string_member (p, "model"),   ==, "gpt");
  g_assert_cmpstr (json_object_get_string_member (p, "api_key"), ==, "sk-xyz");
  g_object_unref (s);
  g_unsetenv ("LLMGHOST_TEST_KEY");
}

static void
test_parse_interpolates_inside_arrays (void)
{
  /* ${ENV} inside a string nested in an array (the generic backend's
   * request_template shape) must be interpolated, not passed through
   * verbatim. Regression: interpolate_object used to skip arrays. */
  g_setenv ("LLMGHOST_TEST_VAR", "VALUE", TRUE);
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backends\":{\"generic\":{\"request_template\":"
      "{\"messages\":[{\"content\":\"a-${LLMGHOST_TEST_VAR}-b\"}]}}}}");
  JsonObject *p = llm_ghost_settings_get_backend_params (s, "generic");
  g_assert_nonnull (p);
  JsonObject *tmpl = json_object_get_object_member (p, "request_template");
  JsonArray  *msgs = json_object_get_array_member (tmpl, "messages");
  JsonObject *m0   = json_array_get_object_element (msgs, 0);
  g_assert_cmpstr (json_object_get_string_member (m0, "content"), ==, "a-VALUE-b");
  g_object_unref (s);
  g_unsetenv ("LLMGHOST_TEST_VAR");
}

static void
test_parse_underscore_key_ignored (void)
{
  /* "_help" is present but never interpreted; parsing still succeeds. */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"_help\":\"hello\",\"backend\":\"openai\"}");
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "openai");
  g_object_unref (s);
}

static void
test_parse_malformed_uses_defaults (void)
{
  /* Built-in default backend is "ollama". The parse warning plus the
   * interpolation warnings for the default template's three unset vars
   * (${VARS}, ${OPENAI_API_KEY}, ${MISTRAL_API_KEY}) are expected. Unset them
   * so the warning set is deterministic regardless of the ambient environment. */
  g_unsetenv ("VARS");
  g_unsetenv ("OPENAI_API_KEY");
  g_unsetenv ("MISTRAL_API_KEY");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "parse error:*");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("this is not json {");
  g_test_assert_expected_messages ();
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "ollama");
  g_object_unref (s);
}

/* Returns a newly-allocated path to a fresh temp settings file seeded with
 * @contents. Caller frees the path; the file lives in a temp dir that the test
 * intentionally leaves behind (cheap, and avoids teardown races). */
static char *
write_temp_settings (const char *contents)
{
  GError *error = NULL;
  char *dir = g_dir_make_tmp ("llmghost-settings-XXXXXX", &error);
  g_assert_no_error (error);
  char *path = g_build_filename (dir, "settings.json", NULL);
  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
  g_free (dir);
  return path;
}

static void
test_autowrite_default (void)
{
  /* A non-existent file → the default template is written, then loaded. Loading
   * the default interpolates its 3 unset template vars, emitting 3 warnings. */
  g_unsetenv ("VARS");
  g_unsetenv ("OPENAI_API_KEY");
  g_unsetenv ("MISTRAL_API_KEY");

  GError *error = NULL;
  char *dir = g_dir_make_tmp ("llmghost-settings-XXXXXX", &error);
  g_assert_no_error (error);
  char *path = g_build_filename (dir, "settings.json", NULL);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*not set*");
  LlmGhostSettings *s = llm_ghost_settings_new (path);
  g_test_assert_expected_messages ();

  g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));            /* written */
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "ollama");

  g_object_unref (s);
  g_free (path);
  g_free (dir);
}

static void
on_changed_count (LlmGhostSettings *s, gpointer data)
{
  (void) s;
  (*(int *) data)++;
}

static void
test_reload_updates_and_signals (void)
{
  /* Clean JSON (no unset vars) → no warnings on this path. */
  char *path = write_temp_settings ("{\"backend\":\"mistral\"}");
  LlmGhostSettings *s = llm_ghost_settings_new (path);
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "mistral");

  int changed = 0;
  g_signal_connect (s, "changed", G_CALLBACK (on_changed_count), &changed);

  GError *error = NULL;
  g_assert_true (g_file_set_contents (path, "{\"backend\":\"openai\"}", -1, &error));
  g_assert_no_error (error);

  _llm_ghost_settings_reload (s);
  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "openai");
  g_assert_cmpint (changed, ==, 1);

  g_object_unref (s);
  g_free (path);
}

static void
test_reload_broken_keeps_last_good (void)
{
  char *path = write_temp_settings ("{\"backend\":\"openai\"}");
  LlmGhostSettings *s = llm_ghost_settings_new (path);

  int changed = 0;
  g_signal_connect (s, "changed", G_CALLBACK (on_changed_count), &changed);

  GError *error = NULL;
  g_assert_true (g_file_set_contents (path, "broken {", -1, &error));
  g_assert_no_error (error);

  /* Broken JSON on reload emits a parse-error warning then a keep-last warning. */
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "parse error:*");
  g_test_expect_message ("llmghost-settings", G_LOG_LEVEL_WARNING, "*keeping last config*");
  _llm_ghost_settings_reload (s);
  g_test_assert_expected_messages ();

  g_assert_cmpstr (llm_ghost_settings_get_active_backend (s), ==, "openai"); /* unchanged */
  g_assert_cmpint (changed, ==, 0);                                /* no signal */

  g_object_unref (s);
  g_free (path);
}

static void
test_factory_ollama (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"ollama\","
    "\"backends\":{\"ollama\":{\"host\":\"h\",\"port\":1,\"model\":\"m\",\"tokens\":\"Qwen\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OLLAMA_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_openai (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"openai\","
    "\"backends\":{\"openai\":{\"base_url\":\"http://x/v1\",\"model\":\"m\",\"mode\":\"chat\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OPENAI_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_mistral (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"mistral\","
    "\"backends\":{\"mistral\":{\"base_url\":\"http://x/v1\",\"model\":\"m\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_MISTRAL_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_unknown_falls_back_to_ollama (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"backend\":\"frobnicate\"}");
  g_test_expect_message ("llmghost-factory", G_LOG_LEVEL_WARNING, "*unknown backend*");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_test_assert_expected_messages ();
  g_assert_true (LLM_GHOST_IS_OLLAMA_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_missing_params_ok (void)
{
  /* No "backends" object at all → factory still builds ollama defaults. */
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string ("{\"backend\":\"ollama\"}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_OLLAMA_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static void
test_factory_generic (void)
{
  LlmGhostSettings *s = _llm_ghost_settings_new_from_string (
    "{\"backend\":\"generic\","
    "\"backends\":{\"generic\":{"
      "\"url\":\"http://x/v1\","
      "\"headers\":{\"x-api-key\":\"k\"},"
      "\"model\":\"m\","
      "\"request_template\":{\"messages\":[{\"content\":\"{{prefix}}\"}]},"
      "\"response_path\":\"content.0.text\"}}}");
  LlmGhostBackend *b = llm_ghost_backend_new_from_settings (s);
  g_assert_true (LLM_GHOST_IS_GENERIC_BACKEND (b));
  g_object_unref (b);
  g_object_unref (s);
}

static JsonObject *
raw_object (const char *json)
{
  JsonParser *parser = json_parser_new ();
  g_assert_true (json_parser_load_from_data (parser, json, -1, NULL));
  JsonObject *o = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  return o;
}

static gboolean
strv_contains (char **v, const char *s)
{
  for (int i = 0; v[i] != NULL; i++)
    if (g_strcmp0 (v[i], s) == 0)
      return TRUE;
  return FALSE;
}

static void
test_collect_refs_basic (void)
{
  JsonObject *o = raw_object (
    "{\"a\":\"${secret:one}\",\"b\":{\"c\":\"x ${secret:two} y\"},"
     "\"d\":[\"${secret:one}\",\"${ENV_X}\"]}");
  char **refs = _llm_ghost_settings_collect_secret_refs (o);
  g_assert_cmpint (g_strv_length (refs), ==, 2);     /* one, two — deduped */
  g_assert_true (strv_contains (refs, "one"));
  g_assert_true (strv_contains (refs, "two"));
  g_assert_false (strv_contains (refs, "ENV_X"));     /* env vars are not secrets */
  g_strfreev (refs);
  json_object_unref (o);
}

static void
test_collect_refs_none (void)
{
  JsonObject *o = raw_object ("{\"a\":\"plain\",\"b\":\"${ENV_ONLY}\"}");
  char **refs = _llm_ghost_settings_collect_secret_refs (o);
  g_assert_cmpint (g_strv_length (refs), ==, 0);
  g_assert_null (refs[0]);
  g_strfreev (refs);
  json_object_unref (o);
}

static void
test_collect_refs_null (void)
{
  char **refs = _llm_ghost_settings_collect_secret_refs (NULL);
  g_assert_nonnull (refs);
  g_assert_null (refs[0]);
  g_strfreev (refs);
}

static void
test_touch_preserves_content (void)
{
  char *path = write_temp_settings ("{\"backend\":\"openai\"}");
  GError *error = NULL;
  g_assert_true (llm_ghost_settings_touch (path, &error));
  g_assert_no_error (error);

  char *data = NULL;
  g_assert_true (g_file_get_contents (path, &data, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (data, ==, "{\"backend\":\"openai\"}");

  g_free (data);
  g_free (path);
}

static void
test_touch_missing_file_errors (void)
{
  GError *error = NULL;
  g_assert_false (llm_ghost_settings_touch ("/nonexistent/llmghost/x.json", &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
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
  g_test_add_func ("/settings/interpolate/secret-found",   test_interpolate_secret_found);
  g_test_add_func ("/settings/interpolate/secret-missing", test_interpolate_secret_missing);
  g_test_add_func ("/settings/interpolate/secret-env",     test_interpolate_secret_and_env_coexist);
  g_test_add_func ("/settings/interpolate/secret-array",   test_interpolate_secret_in_array);
  g_test_add_func ("/settings/parse/active-backend",         test_parse_active_backend);
  g_test_add_func ("/settings/parse/active-backend-default", test_parse_active_backend_default);
  g_test_add_func ("/settings/parse/unknown-passthrough",    test_parse_unknown_backend_passthrough);
  g_test_add_func ("/settings/parse/debounce",               test_parse_debounce);
  g_test_add_func ("/settings/parse/debounce-absent",        test_parse_debounce_absent);
  g_test_add_func ("/settings/parse/max-lines",              test_parse_max_lines);
  g_test_add_func ("/settings/parse/max-lines-absent",       test_parse_max_lines_absent);
  g_test_add_func ("/settings/parse/params-interpolated",    test_parse_backend_params_interpolated);
  g_test_add_func ("/settings/parse/interpolate-in-arrays",  test_parse_interpolates_inside_arrays);
  g_test_add_func ("/settings/parse/underscore-ignored",     test_parse_underscore_key_ignored);
  g_test_add_func ("/settings/parse/malformed-defaults",     test_parse_malformed_uses_defaults);
  g_test_add_func ("/settings/file/autowrite-default",   test_autowrite_default);
  g_test_add_func ("/settings/file/reload-updates",      test_reload_updates_and_signals);
  g_test_add_func ("/settings/file/reload-broken",       test_reload_broken_keeps_last_good);
  g_test_add_func ("/settings/factory/ollama",         test_factory_ollama);
  g_test_add_func ("/settings/factory/openai",         test_factory_openai);
  g_test_add_func ("/settings/factory/mistral",        test_factory_mistral);
  g_test_add_func ("/settings/factory/unknown",        test_factory_unknown_falls_back_to_ollama);
  g_test_add_func ("/settings/factory/missing-params", test_factory_missing_params_ok);
  g_test_add_func ("/settings/factory/generic",        test_factory_generic);
  g_test_add_func ("/settings/secret-refs/basic", test_collect_refs_basic);
  g_test_add_func ("/settings/secret-refs/none",  test_collect_refs_none);
  g_test_add_func ("/settings/secret-refs/null",  test_collect_refs_null);
  g_test_add_func ("/settings/touch/preserves", test_touch_preserves_content);
  g_test_add_func ("/settings/touch/missing",   test_touch_missing_file_errors);
  return g_test_run ();
}
