#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-fim-tokens.h"
#include "llmghost-ollama-backend-internal.h"

static JsonObject *
parse_object (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  g_assert_true (json_parser_load_from_data (parser, json, -1, &error));
  g_assert_no_error (error);
  JsonNode *root = json_parser_get_root (parser);
  g_assert_true (JSON_NODE_HOLDS_OBJECT (root));
  JsonObject *obj = json_object_ref (json_node_get_object (root));
  g_object_unref (parser);
  return obj;
}

static void
test_body_top_level_fields (void)
{
  char *body = _llm_ghost_ollama_build_request_body (
      "my-model", llm_ghost_fim_tokens_qwen (), "int main", "}", 64, 0.2, TRUE);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"), ==, "my-model");
  g_assert_true  (json_object_get_boolean_member (obj, "raw"));
  g_assert_false (json_object_get_boolean_member (obj, "stream"));
  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==,
                   "<|fim_prefix|>int main<|fim_suffix|>}<|fim_middle|>");

  json_object_unref (obj);
  g_free (body);
}

static void
test_body_options_and_stops (void)
{
  char *body = _llm_ghost_ollama_build_request_body (
      "m", llm_ghost_fim_tokens_qwen (), "a", "b", 64, 0.2, TRUE);
  JsonObject *obj = parse_object (body);
  JsonObject *opts = json_object_get_object_member (obj, "options");

  g_assert_cmpint (json_object_get_int_member (opts, "num_predict"), ==, 64);
  g_assert_true (ABS (json_object_get_double_member (opts, "temperature") - 0.2) < 1e-9);

  JsonArray *stop = json_object_get_array_member (opts, "stop");
  g_assert_cmpint (json_array_get_length (stop), ==, 4);   /* "\n" + 3 Qwen stops */
  g_assert_cmpstr (json_array_get_string_element (stop, 0), ==, "\n");
  g_assert_cmpstr (json_array_get_string_element (stop, 1), ==, "<|endoftext|>");
  g_assert_cmpstr (json_array_get_string_element (stop, 2), ==, "<|fim_pad|>");
  g_assert_cmpstr (json_array_get_string_element (stop, 3), ==, "<|im_end|>");

  json_object_unref (obj);
  g_free (body);
}

static void
test_body_null_prefix_suffix (void)
{
  char *body = _llm_ghost_ollama_build_request_body (
      "m", llm_ghost_fim_tokens_qwen (), NULL, NULL, 64, 0.2, TRUE);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==,
                   "<|fim_prefix|><|fim_suffix|><|fim_middle|>");

  json_object_unref (obj);
  g_free (body);
}

static void
test_ollama_stop_single_line (void)
{
  const LlmGhostFimTokens *t = llm_ghost_fim_tokens_qwen ();
  char *on  = _llm_ghost_ollama_build_request_body ("m", t, "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_ollama_build_request_body ("m", t, "p", "s", 64, 0.2, FALSE);
  /* single-line: a bare "\n" appears in the stop array; multi-line: it does not.
   * The family sentinel tokens remain in both. */
  g_assert_nonnull (g_strstr_len (on,  -1, "\"\\n\""));
  g_assert_null    (g_strstr_len (off, -1, "\"\\n\""));
  /* The FIM family sentinels terminate the completion in BOTH modes — only the
   * per-line "\n" stop is dropped for multi-line. */
  g_assert_nonnull (g_strstr_len (off, -1, "<|endoftext|>"));
  g_assert_nonnull (g_strstr_len (off, -1, "<|im_end|>"));
  g_free (on);
  g_free (off);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ollama-body/top-level",   test_body_top_level_fields);
  g_test_add_func ("/ollama-body/options-stops", test_body_options_and_stops);
  g_test_add_func ("/ollama-body/null-prefix-suffix", test_body_null_prefix_suffix);
  g_test_add_func ("/ollama-body/stop-single-line", test_ollama_stop_single_line);
  return g_test_run ();
}
