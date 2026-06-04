/* Minimal GTK3 host that exercises libllmghost against the Ollama backend.
 *
 * Type into the text view; after ~80ms of idle, the controller fires a
 * fill-in-the-middle request to spark-2448 and renders the response as
 * single-line ghost text. Tab accepts, Esc dismisses.
 *
 * Optional environment variables for ad-hoc overrides without rebuilding:
 *   LLMGHOST_OLLAMA_HOST    (default spark-2448)
 *   LLMGHOST_OLLAMA_PORT    (default 11434)
 *   LLMGHOST_OLLAMA_MODEL   (default qwen3-coder-next:latest)
 *   LLMGHOST_OLLAMA_TOKENS  (default Qwen; one of: Qwen, StarCoder, DeepSeek)
 *   LLMGHOST_BACKEND        (default ollama; "openai" or "mistral" for those backends)
 *   LLMGHOST_OPENAI_BASE_URL / _MODEL / _API_KEY / _MODE  (OpenAI backend config)
 *   LLMGHOST_MISTRAL_BASE_URL / _MODEL / _API_KEY         (Mistral backend config)
 */

#include <gtk/gtk.h>
#include <stdlib.h>

#include "llmghost.h"

static void
activate (GtkApplication *app, gpointer user_data)
{
  (void) user_data;

  GtkWidget *window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (Ollama)");
  gtk_window_set_default_size (GTK_WINDOW (window), 720, 480);

  GtkWidget *scroll = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *view   = gtk_text_view_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);

  gtk_container_add (GTK_CONTAINER (scroll), view);
  gtk_container_add (GTK_CONTAINER (window), scroll);

  const char *which = g_getenv ("LLMGHOST_BACKEND");   /* "openai" | "ollama" (default) */
  LlmGhostBackend *backend;

  if (which != NULL && g_ascii_strcasecmp (which, "openai") == 0)
    {
      /* base/model/key/mode all read from LLMGHOST_OPENAI_* by the ctor. */
      backend = llm_ghost_openai_backend_new (NULL, NULL, NULL,
                                              LLM_GHOST_OPENAI_MODE_CHAT);
      gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (OpenAI)");
    }
  else if (which != NULL && g_ascii_strcasecmp (which, "mistral") == 0)
    {
      /* base/model/key all read from LLMGHOST_MISTRAL_* by the ctor. */
      backend = llm_ghost_mistral_backend_new (NULL, NULL, NULL);
      gtk_window_set_title (GTK_WINDOW (window), "llmghost demo (Mistral Codestral)");
    }
  else
    {
      const char *host_env   = g_getenv ("LLMGHOST_OLLAMA_HOST");
      const char *port_env   = g_getenv ("LLMGHOST_OLLAMA_PORT");
      const char *model_env  = g_getenv ("LLMGHOST_OLLAMA_MODEL");
      const char *tokens_env = g_getenv ("LLMGHOST_OLLAMA_TOKENS");

      guint16 port = 0;
      if (port_env != NULL && *port_env != '\0')
        {
          gint64 v = g_ascii_strtoll (port_env, NULL, 10);
          if (v > 0 && v < 65536)
            port = (guint16) v;
        }

      backend = llm_ghost_ollama_backend_new (host_env, port, model_env);

      if (tokens_env != NULL && *tokens_env != '\0')
        {
          const LlmGhostFimTokens *toks = llm_ghost_fim_tokens_lookup_builtin (tokens_env);
          if (toks != NULL)
            llm_ghost_ollama_backend_set_fim_tokens (LLM_GHOST_OLLAMA_BACKEND (backend), toks);
          else
            g_printerr ("llmghost-demo: unknown FIM token set %s; using default (Qwen)\n",
                        tokens_env);
        }
    }

  LlmGhostController *ctrl = llm_ghost_controller_new (GTK_TEXT_VIEW (view), backend);
  g_object_unref (backend);

  /* Tie the controller's lifetime to the window. */
  g_object_set_data_full (G_OBJECT (window), "llmghost-controller",
                          ctrl, g_object_unref);

  gtk_widget_show_all (window);
}

int
main (int argc, char **argv)
{
  GtkApplication *app = gtk_application_new ("io.example.LlmGhostDemo",
                                             G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
