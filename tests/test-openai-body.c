#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-openai-backend-internal.h"

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
test_completions_body (void)
{
  char *body = _llm_ghost_openai_build_completions_body ("m", "int main", "}", 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"),  ==, "m");
  g_assert_cmpstr (json_object_get_string_member (obj, "prompt"), ==, "int main");
  g_assert_cmpstr (json_object_get_string_member (obj, "suffix"), ==, "}");
  g_assert_cmpint (json_object_get_int_member (obj, "max_tokens"), ==, 64);
  g_assert_true (ABS (json_object_get_double_member (obj, "temperature") - 0.2) < 1e-9);
  g_assert_false (json_object_get_boolean_member (obj, "stream"));

  JsonArray *stop = json_object_get_array_member (obj, "stop");
  g_assert_cmpint (json_array_get_length (stop), ==, 1);
  g_assert_cmpstr (json_array_get_string_element (stop, 0), ==, "\n");

  json_object_unref (obj);
  g_free (body);
}

static void
test_chat_body (void)
{
  char *body = _llm_ghost_openai_build_chat_body ("m", "foo(", ")", 64, 0.2);
  JsonObject *obj = parse_object (body);

  g_assert_cmpstr (json_object_get_string_member (obj, "model"), ==, "m");
  JsonArray *msgs = json_object_get_array_member (obj, "messages");
  g_assert_cmpint (json_array_get_length (msgs), ==, 2);

  JsonObject *sys = json_array_get_object_element (msgs, 0);
  g_assert_cmpstr (json_object_get_string_member (sys, "role"), ==, "system");

  JsonObject *usr = json_array_get_object_element (msgs, 1);
  g_assert_cmpstr (json_object_get_string_member (usr, "role"), ==, "user");
  g_assert_cmpstr (json_object_get_string_member (usr, "content"), ==,
                   "<PREFIX>foo(</PREFIX>\n<SUFFIX>)</SUFFIX>");

  json_object_unref (obj);
  g_free (body);
}

static void
check_clean (const char *raw, const char *expect)
{
  char *got = _llm_ghost_openai_clean_chat_completion (raw);
  g_assert_cmpstr (got, ==, expect);
  g_free (got);
}

static void
test_clean_chat_completion (void)
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
test_extract_completions (void)
{
  JsonNode *node = parse_node ("{\"choices\":[{\"text\":\"hello\"}]}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_COMPLETIONS, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "hello");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_chat_cleans (void)
{
  JsonNode *node = parse_node (
      "{\"choices\":[{\"message\":{\"content\":\"```\\nfoo()\\n```\"}}]}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_CHAT, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "foo()");
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_error_object (void)
{
  JsonNode *node = parse_node ("{\"error\":{\"message\":\"bad key\"}}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_CHAT, &error);
  g_assert_null (out);
  g_assert_nonnull (error);
  g_assert_nonnull (g_strstr_len (error->message, -1, "bad key"));
  g_clear_error (&error);
  json_node_unref (node);
}

static void
test_extract_empty_choices (void)
{
  JsonNode *node = parse_node ("{\"choices\":[]}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_COMPLETIONS, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");   /* no suggestion */
  g_free (out);
  json_node_unref (node);
}

static void
test_extract_error_string (void)
{
  /* Some compat servers return error as a bare string rather than an object. */
  JsonNode *node = parse_node ("{\"error\":\"rate limited\"}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_CHAT, &error);
  g_assert_null (out);
  g_assert_nonnull (error);
  g_assert_nonnull (g_strstr_len (error->message, -1, "rate limited"));
  g_clear_error (&error);
  json_node_unref (node);
}

static void
test_extract_missing_choices (void)
{
  /* No "choices" member at all → no suggestion, no error. */
  JsonNode *node = parse_node ("{}");
  GError *error = NULL;
  char *out = _llm_ghost_openai_extract_completion (node, LLM_GHOST_OPENAI_MODE_COMPLETIONS, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (out, ==, "");
  g_free (out);
  json_node_unref (node);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/openai-body/completions",      test_completions_body);
  g_test_add_func ("/openai-body/chat",             test_chat_body);
  g_test_add_func ("/openai-body/clean",            test_clean_chat_completion);
  g_test_add_func ("/openai-body/extract-completions", test_extract_completions);
  g_test_add_func ("/openai-body/extract-chat",     test_extract_chat_cleans);
  g_test_add_func ("/openai-body/extract-error",    test_extract_error_object);
  g_test_add_func ("/openai-body/extract-error-string", test_extract_error_string);
  g_test_add_func ("/openai-body/extract-empty",    test_extract_empty_choices);
  g_test_add_func ("/openai-body/extract-missing-choices", test_extract_missing_choices);
  return g_test_run ();
}
