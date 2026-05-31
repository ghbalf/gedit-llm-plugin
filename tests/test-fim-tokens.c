#include <glib.h>
#include "llmghost-fim-tokens.h"

static void
test_new_copies_fields (void)
{
  const char *stops[] = { "<|endoftext|>", NULL };
  LlmGhostFimTokens *t =
      llm_ghost_fim_tokens_new ("X", "<p>", "<s>", "<m>",
                                (const char * const *) stops);
  g_assert_nonnull (t);
  g_assert_cmpstr (t->name,       ==, "X");
  g_assert_cmpstr (t->prefix_tok, ==, "<p>");
  g_assert_cmpstr (t->suffix_tok, ==, "<s>");
  g_assert_cmpstr (t->middle_tok, ==, "<m>");
  g_assert_nonnull (t->stop_tokens);
  g_assert_cmpstr (t->stop_tokens[0], ==, "<|endoftext|>");
  g_assert_null   (t->stop_tokens[1]);
  llm_ghost_fim_tokens_free (t);
}

static void
test_copy_is_independent (void)
{
  const char *stops[] = { "S", NULL };
  LlmGhostFimTokens *orig =
      llm_ghost_fim_tokens_new ("orig", "p", "s", "m",
                                (const char * const *) stops);
  LlmGhostFimTokens *dup = llm_ghost_fim_tokens_copy (orig);
  llm_ghost_fim_tokens_free (orig);   /* free original; copy must survive */

  g_assert_nonnull (dup);
  g_assert_cmpstr (dup->name,          ==, "orig");
  g_assert_cmpstr (dup->prefix_tok,    ==, "p");
  g_assert_cmpstr (dup->suffix_tok,    ==, "s");
  g_assert_cmpstr (dup->middle_tok,    ==, "m");
  g_assert_cmpstr (dup->stop_tokens[0], ==, "S");
  g_assert_null   (dup->stop_tokens[1]);
  llm_ghost_fim_tokens_free (dup);
}

static void
test_free_null_is_safe (void)
{
  llm_ghost_fim_tokens_free (NULL);   /* must not crash */
  g_assert_null (llm_ghost_fim_tokens_copy (NULL));
}

static void
test_builtins_present_and_distinct (void)
{
  const LlmGhostFimTokens *q = llm_ghost_fim_tokens_qwen ();
  const LlmGhostFimTokens *s = llm_ghost_fim_tokens_starcoder ();
  const LlmGhostFimTokens *d = llm_ghost_fim_tokens_deepseek ();

  g_assert_nonnull (q);
  g_assert_nonnull (s);
  g_assert_nonnull (d);
  g_assert_cmpstr (q->name, ==, "Qwen");
  g_assert_cmpstr (s->name, ==, "StarCoder");
  g_assert_cmpstr (d->name, ==, "DeepSeek");
  g_assert_cmpstr (q->prefix_tok, ==, "<|fim_prefix|>");
  g_assert_cmpstr (s->prefix_tok, ==, "<fim_prefix>");
  /* distinct sentinels across families */
  g_assert_cmpstr (q->prefix_tok, !=, s->prefix_tok);
  g_assert_cmpstr (q->prefix_tok, !=, d->prefix_tok);
}

static void
test_builtins_list_is_null_terminated (void)
{
  const LlmGhostFimTokens * const *all = llm_ghost_fim_tokens_builtins ();
  g_assert_nonnull (all);
  guint n = 0;
  while (all[n] != NULL)
    n++;
  g_assert_cmpint (n, ==, 3);   /* Qwen + StarCoder + DeepSeek */
  /* Order is the contract: callers (e.g. a settings dropdown) rely on it. */
  g_assert_cmpstr (all[0]->name, ==, "Qwen");
  g_assert_cmpstr (all[1]->name, ==, "StarCoder");
  g_assert_cmpstr (all[2]->name, ==, "DeepSeek");
}

static void
test_lookup_builtin_case_insensitive (void)
{
  g_assert_cmpstr (llm_ghost_fim_tokens_lookup_builtin ("qwen")->name, ==, "Qwen");
  g_assert_cmpstr (llm_ghost_fim_tokens_lookup_builtin ("QWEN")->name, ==, "Qwen");
  g_assert_cmpstr (llm_ghost_fim_tokens_lookup_builtin ("Qwen")->name, ==, "Qwen");
  g_assert_null   (llm_ghost_fim_tokens_lookup_builtin ("nope"));
  g_assert_null   (llm_ghost_fim_tokens_lookup_builtin (NULL));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/fim-tokens/new-copies-fields",     test_new_copies_fields);
  g_test_add_func ("/fim-tokens/copy-independent",      test_copy_is_independent);
  g_test_add_func ("/fim-tokens/free-null-safe",        test_free_null_is_safe);
  g_test_add_func ("/fim-tokens/builtins-distinct",     test_builtins_present_and_distinct);
  g_test_add_func ("/fim-tokens/builtins-list",         test_builtins_list_is_null_terminated);
  g_test_add_func ("/fim-tokens/lookup-case-insensitive", test_lookup_builtin_case_insensitive);
  return g_test_run ();
}
