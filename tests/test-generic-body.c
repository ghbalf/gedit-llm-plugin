#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-generic-backend-internal.h"

/* Parse a template literal into a JsonObject (caller unrefs). */
static JsonObject *
obj_from (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonObject *obj = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  return obj;
}

/* Build a body, then parse the result back for assertions (caller unrefs). */
static JsonObject *
build_and_parse (JsonObject *tmpl, const char *prefix, const char *suffix, const char *model)
{
  char *body = _llm_ghost_generic_build_body (tmpl, prefix, suffix, model);
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, body, -1, &error));   /* valid JSON */
  g_assert_no_error (error);
  JsonObject *out = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  g_free (body);
  return out;
}

static void
test_build_simple (void)
{
  JsonObject *t = obj_from ("{\"p\":\"{{prefix}}\",\"s\":\"{{suffix}}\"}");
  JsonObject *o = build_and_parse (t, "AAA", "BBB", NULL);
  g_assert_cmpstr (json_object_get_string_member (o, "p"), ==, "AAA");
  g_assert_cmpstr (json_object_get_string_member (o, "s"), ==, "BBB");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_model (void)
{
  JsonObject *t = obj_from ("{\"m\":\"{{model}}\"}");
  JsonObject *o = build_and_parse (t, "", "", "claude-x");
  g_assert_cmpstr (json_object_get_string_member (o, "m"), ==, "claude-x");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_escaping (void)
{
  /* A prefix with a quote, newline, and backslash must round-trip exactly —
   * proof that we substitute structurally and let json-glib do the escaping. */
  JsonObject *t = obj_from ("{\"p\":\"{{prefix}}\"}");
  JsonObject *o = build_and_parse (t, "a\"b\nc\\d", "", NULL);
  g_assert_cmpstr (json_object_get_string_member (o, "p"), ==, "a\"b\nc\\d");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_nested (void)
{
  JsonObject *t = obj_from (
    "{\"messages\":[{\"role\":\"user\",\"content\":\"X{{prefix}}Y\"}]}");
  JsonObject *o = build_and_parse (t, "P", "", NULL);
  JsonArray *msgs = json_object_get_array_member (o, "messages");
  JsonObject *m0 = json_array_get_object_element (msgs, 0);
  g_assert_cmpstr (json_object_get_string_member (m0, "content"), ==, "XPY");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_unknown_placeholder_verbatim (void)
{
  JsonObject *t = obj_from ("{\"k\":\"{{bogus}}\"}");
  JsonObject *o = build_and_parse (t, "P", "S", "M");
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "{{bogus}}");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_unclosed_braces_verbatim (void)
{
  /* "{{" with no closing "}}" is reproduced verbatim (no token consumed). */
  JsonObject *t = obj_from ("{\"k\":\"a {{ b\"}");
  JsonObject *o = build_and_parse (t, "P", "S", "M");
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "a {{ b");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_empty_token_verbatim (void)
{
  /* "{{}}" is an empty (thus unknown) token — copied verbatim. */
  JsonObject *t = obj_from ("{\"k\":\"x{{}}y\"}");
  JsonObject *o = build_and_parse (t, "P", "S", "M");
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "x{{}}y");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_single_pass_safety (void)
{
  /* prefix value contains "{{suffix}}" — it must NOT be re-substituted. */
  JsonObject *t = obj_from ("{\"k\":\"{{prefix}}\"}");
  JsonObject *o = build_and_parse (t, "a{{suffix}}b", "S", NULL);
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "a{{suffix}}b");
  json_object_unref (o);
  json_object_unref (t);
}

static void
test_build_no_placeholders (void)
{
  JsonObject *t = obj_from ("{\"k\":\"v\",\"n\":5}");
  JsonObject *o = build_and_parse (t, "P", "S", "M");
  g_assert_cmpstr (json_object_get_string_member (o, "k"), ==, "v");
  g_assert_cmpint (json_object_get_int_member (o, "n"), ==, 5);
  json_object_unref (o);
  json_object_unref (t);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/generic-body/simple",            test_build_simple);
  g_test_add_func ("/generic-body/model",             test_build_model);
  g_test_add_func ("/generic-body/escaping",          test_build_escaping);
  g_test_add_func ("/generic-body/nested",            test_build_nested);
  g_test_add_func ("/generic-body/unknown-verbatim",  test_build_unknown_placeholder_verbatim);
  g_test_add_func ("/generic-body/unclosed-braces",   test_build_unclosed_braces_verbatim);
  g_test_add_func ("/generic-body/empty-token",       test_build_empty_token_verbatim);
  g_test_add_func ("/generic-body/single-pass-safety", test_build_single_pass_safety);
  g_test_add_func ("/generic-body/no-placeholders",   test_build_no_placeholders);
  return g_test_run ();
}
