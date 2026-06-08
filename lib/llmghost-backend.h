#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_BACKEND (llm_ghost_backend_get_type())
G_DECLARE_INTERFACE (LlmGhostBackend, llm_ghost_backend, LLM_GHOST, BACKEND, GObject)

/* Emitted by streaming backends as completion text accumulates. Signature:
 *   void (*) (LlmGhostBackend *self, const char *accumulated_text)
 * request_finish() still returns the full completion; non-streaming backends
 * never emit this. */
#define LLM_GHOST_BACKEND_SIGNAL_PARTIAL_DATA "partial-data"

struct _LlmGhostBackendInterface
{
  GTypeInterface g_iface;

  void   (*request)        (LlmGhostBackend     *self,
                            const char          *prefix,
                            const char          *suffix,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data);

  char * (*request_finish) (LlmGhostBackend     *self,
                            GAsyncResult        *result,
                            GError             **error);
};

void   llm_ghost_backend_request        (LlmGhostBackend     *self,
                                         const char          *prefix,
                                         const char          *suffix,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);

char * llm_ghost_backend_request_finish (LlmGhostBackend     *self,
                                         GAsyncResult        *result,
                                         GError             **error);

G_END_DECLS
