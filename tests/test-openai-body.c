#include <glib.h>
#include <json-glib/json-glib.h>
#include "llmghost-openai-backend-internal.h"
#include "llmghost-http-util.h"

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
  char *body = _llm_ghost_openai_build_completions_body ("m", "int main", "}", 64, 0.2, FALSE);
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
  char *body = _llm_ghost_openai_build_chat_body ("m", "foo(", ")", 64, 0.2, FALSE);
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

static void
test_chat_body_stream_flag (void)
{
  char *on  = _llm_ghost_openai_build_chat_body ("m", "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_openai_build_chat_body ("m", "p", "s", 64, 0.2, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stream\":true"));
  g_assert_nonnull (g_strstr_len (off, -1, "\"stream\":false"));
  g_free (on);
  g_free (off);
}

static void
test_completions_body_stream_flag (void)
{
  char *on  = _llm_ghost_openai_build_completions_body ("m", "p", "s", 64, 0.2, TRUE);
  char *off = _llm_ghost_openai_build_completions_body ("m", "p", "s", 64, 0.2, FALSE);
  g_assert_nonnull (g_strstr_len (on,  -1, "\"stream\":true"));
  g_assert_nonnull (g_strstr_len (off, -1, "\"stream\":false"));
  g_free (on);
  g_free (off);
}

static void
test_delta_null_content_is_empty (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"content\":null}}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, NULL);
  g_assert_cmpstr (d, ==, "");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_chat (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"content\":\"He\"}}]}");
  GError *e = NULL;
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, &e);
  g_assert_no_error (e);
  g_assert_cmpstr (d, ==, "He");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_completions (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"text\":\"xy\"}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_COMPLETIONS, NULL);
  g_assert_cmpstr (d, ==, "xy");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_role_only_is_empty (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, NULL);
  g_assert_cmpstr (d, ==, "");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_finish_chunk_is_empty (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}");
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, NULL);
  g_assert_cmpstr (d, ==, "");
  g_free (d);
  json_node_unref (n);
}

static void
test_delta_error_member (void)
{
  JsonNode *n = _llm_ghost_http_parse_json (
    "{\"error\":{\"message\":\"boom\"}}");
  GError *e = NULL;
  char *d = _llm_ghost_openai_extract_delta (n, LLM_GHOST_OPENAI_MODE_CHAT, &e);
  g_assert_null (d);
  g_assert_error (e, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (g_strstr_len (e->message, -1, "boom"));
  g_clear_error (&e);
  json_node_unref (n);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/openai-body/completions",      test_completions_body);
  g_test_add_func ("/openai-body/chat",             test_chat_body);
  g_test_add_func ("/openai-body/extract-completions", test_extract_completions);
  g_test_add_func ("/openai-body/extract-chat",     test_extract_chat_cleans);
  g_test_add_func ("/openai-body/extract-error",    test_extract_error_object);
  g_test_add_func ("/openai-body/extract-error-string", test_extract_error_string);
  g_test_add_func ("/openai-body/extract-empty",    test_extract_empty_choices);
  g_test_add_func ("/openai-body/extract-missing-choices", test_extract_missing_choices);
  g_test_add_func ("/openai/chat-body-stream-flag", test_chat_body_stream_flag);
  g_test_add_func ("/openai/completions-body-stream-flag", test_completions_body_stream_flag);
  g_test_add_func ("/openai/delta-chat",            test_delta_chat);
  g_test_add_func ("/openai/delta-null-content",    test_delta_null_content_is_empty);
  g_test_add_func ("/openai/delta-completions",     test_delta_completions);
  g_test_add_func ("/openai/delta-role-only",       test_delta_role_only_is_empty);
  g_test_add_func ("/openai/delta-finish-chunk",    test_delta_finish_chunk_is_empty);
  g_test_add_func ("/openai/delta-error-member",    test_delta_error_member);
  return g_test_run ();
}
