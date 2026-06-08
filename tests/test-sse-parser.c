#include <glib.h>
#include <string.h>
#include "llmghost-sse-parser.h"

static GPtrArray *
feed_all (const char *const *chunks, gboolean finish)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  LlmGhostSseParser *p = _llm_ghost_sse_parser_new ();
  for (int i = 0; chunks[i] != NULL; i++)
    _llm_ghost_sse_parser_feed (p, chunks[i], strlen (chunks[i]), out);
  if (finish)
    _llm_ghost_sse_parser_finish (p, out);
  _llm_ghost_sse_parser_free (p);
  return out;
}

static void
test_single_event (void)
{
  const char *chunks[] = { "data: hello\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "hello");
  g_ptr_array_unref (out);
}

static void
test_done_passthrough (void)
{
  const char *chunks[] = { "data: [DONE]\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "[DONE]");
  g_ptr_array_unref (out);
}

static void
test_split_across_chunks (void)
{
  const char *chunks[] = { "data: hel", "lo\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "hello");
  g_ptr_array_unref (out);
}

static void
test_multiple_events_one_chunk (void)
{
  const char *chunks[] = { "data: a\n\ndata: b\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "a");
  g_assert_cmpstr (g_ptr_array_index (out, 1), ==, "b");
  g_ptr_array_unref (out);
}

static void
test_crlf (void)
{
  const char *chunks[] = { "data: x\r\n\r\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "x");
  g_ptr_array_unref (out);
}

static void
test_comment_and_event_lines_ignored (void)
{
  const char *chunks[] = { ": ping\n\nevent: msg\ndata: y\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "y");
  g_ptr_array_unref (out);
}

static void
test_multi_data_concat (void)
{
  const char *chunks[] = { "data: a\ndata: b\n\n", NULL };
  GPtrArray *out = feed_all (chunks, FALSE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "a\nb");
  g_ptr_array_unref (out);
}

static void
test_finish_flushes_unterminated (void)
{
  const char *chunks[] = { "data: z\n", NULL };
  GPtrArray *out = feed_all (chunks, TRUE);
  g_assert_cmpuint (out->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (out, 0), ==, "z");
  g_ptr_array_unref (out);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/sse/single-event",        test_single_event);
  g_test_add_func ("/sse/done-passthrough",     test_done_passthrough);
  g_test_add_func ("/sse/split-across-chunks",  test_split_across_chunks);
  g_test_add_func ("/sse/multiple-events",      test_multiple_events_one_chunk);
  g_test_add_func ("/sse/crlf",                 test_crlf);
  g_test_add_func ("/sse/comment-event-ignored", test_comment_and_event_lines_ignored);
  g_test_add_func ("/sse/multi-data-concat",    test_multi_data_concat);
  g_test_add_func ("/sse/finish-flushes",       test_finish_flushes_unterminated);
  return g_test_run ();
}
