#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-mistral-backend-internal.h"

static JsonObject *
parse_object (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonObject *obj = json_object_ref (json_node_get_object (json_parser_get_root (parser)));
  g_object_unref (parser);
  return obj;
}

static JsonNode *
parse_node (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonNode *node = json_node_copy (json_parser_get_root (parser));
  g_object_unref (parser);
  return node;
}

static void
test_fim_body (void)
{
  char *body = _llm_ghost_mistral_build_fim_body ("codestral-latest", "int main", "}", 64, 0.2, TRUE);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"),  ==, "codestral-latest");
  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==, "int main");
  g_assert_cmpstr (json_object_get_string_member (obj, "suffix"), ==, "}");
  g_assert_cmpint (json_object_get_int_member (obj, "max_tokens"), ==, 64);
  g_assert_true (ABS (json_object_get_double_member (obj, "temperature") - 0.2) < 1e-9);

  JsonArray *stop = json_object_get_array_member (obj, "stop");
  g_assert_cmpint (json_array_get_length (stop), ==, 1);
  g_assert_cmpstr (json_array_get_string_element (stop, 0), ==, "\n");

  json_object_unref (obj);
  g_free (body);
}

static void
test_extract_message_content (void)
{
  JsonNode *node = parse_node (
      "{\"choices\":[{\"message\":{\"content\":\"sum(a, b)\"}}]}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "sum(a, b)");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_text_fallback (void)
{
  /* No "message" → fall back to choices[0].text. */
  JsonNode *node = parse_node ("{\"choices\":[{\"text\":\"fallback\"}]}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "fallback");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_empty_choices (void)
{
  JsonNode *node = parse_node ("{\"choices\":[]}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_missing_choices (void)
{
  JsonNode *node = parse_node ("{}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_error_object (void)
{
  JsonNode *node = parse_node ("{\"error\":{\"message\":\"unauthorized\"}}");
  GError *error = NULL;
  char *out = _llm_ghost_mistral_extract_completion (node, &error);
  g_assert_null (out);
  g_assert_nonnull (error);
  g_assert_nonnull (g_strstr_len (error->message, -1, "unauthorized"));
  g_clear_error (&error);
  json_node_unref (node);
}

static void
test_mistral_stop_single_line (void)
{
  char *on  = _llm_ghost_mistral_build_fim_body ("m", "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_mistral_build_fim_body ("m", "p", "s", 64, 0.2, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stop\""));   /* single-line keeps stop */
  g_assert_null    (g_strstr_len (off, -1, "\"stop\""));   /* multi-line omits it */
  g_free (on);
  g_free (off);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/mistral-body/fim",              test_fim_body);
  g_test_add_func ("/mistral-body/extract-message",  test_extract_message_content);
  g_test_add_func ("/mistral-body/extract-text-fallback", test_extract_text_fallback);
  g_test_add_func ("/mistral-body/extract-empty",    test_extract_empty_choices);
  g_test_add_func ("/mistral-body/extract-missing",  test_extract_missing_choices);
  g_test_add_func ("/mistral-body/extract-error",    test_extract_error_object);
  g_test_add_func ("/mistral-body/stop-single-line", test_mistral_stop_single_line);
  return g_test_run ();
}
