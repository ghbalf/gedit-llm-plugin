#define G_LOG_DOMAIN "llmghost-backend"

#include "llmghost-backend.h"
#include "llmghost-backend-internal.h"

G_DEFINE_INTERFACE (LlmGhostBackend, llm_ghost_backend, G_TYPE_OBJECT)

static void
llm_ghost_backend_default_init (LlmGhostBackendInterface *iface)
{
  (void) iface;

  g_signal_new (LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA,
                LLM_GHOST_TYPE_BACKEND,
                G_SIGNAL_RUN_LAST,
                0, NULL, NULL, NULL,
                G_TYPE_NONE, 1, G_TYPE_STRING);
}

void
_llm_ghost_backend_emit_partial_data (LlmGhostBackend *self,
                                      const char      *accumulated)
{
  g_return_if_fail (LLM_GHOST_IS_BACKEND (self));
  g_signal_emit_by_name (self, LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA, accumulated);
}

void
llm_ghost_backend_request (LlmGhostBackend     *self,
                           const char          *prefix,
                           const char          *suffix,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  g_return_if_fail (LLM_GHOST_IS_BACKEND (self));
  LLM_GHOST_BACKEND_GET_IFACE (self)->request (self, prefix, suffix,
                                               cancellable, callback, user_data);
}

char *
llm_ghost_backend_request_finish (LlmGhostBackend  *self,
                                  GAsyncResult     *result,
                                  GError          **error)
{
  g_return_val_if_fail (LLM_GHOST_IS_BACKEND (self), NULL);
  return LLM_GHOST_BACKEND_GET_IFACE (self)->request_finish (self, result, error);
}
