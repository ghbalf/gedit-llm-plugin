#include "llmghost-fake-backend.h"

struct _LlmGhostFakeBackend
{
  GObject  parent_instance;
  char    *canned;
};

static void llm_ghost_fake_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (LlmGhostFakeBackend, llm_ghost_fake_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                llm_ghost_fake_backend_iface_init))

static void
llm_ghost_fake_backend_finalize (GObject *object)
{
  LlmGhostFakeBackend *self = LLM_GHOST_FAKE_BACKEND (object);
  g_clear_pointer (&self->canned, g_free);
  G_OBJECT_CLASS (llm_ghost_fake_backend_parent_class)->finalize (object);
}

static void
llm_ghost_fake_backend_class_init (LlmGhostFakeBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_fake_backend_finalize;
}

static void
llm_ghost_fake_backend_init (LlmGhostFakeBackend *self)
{
  (void) self;
}

static void
fake_request (LlmGhostBackend     *backend,
              const char          *prefix,
              const char          *suffix,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
  LlmGhostFakeBackend *self = LLM_GHOST_FAKE_BACKEND (backend);
  GTask *task = g_task_new (self, cancellable, callback, user_data);

  (void) prefix;
  (void) suffix;

  g_task_return_pointer (task,
                         g_strdup (self->canned ? self->canned
                                                : "// hello, ghost!"),
                         g_free);
  g_object_unref (task);
}

static char *
fake_request_finish (LlmGhostBackend  *backend,
                     GAsyncResult     *result,
                     GError          **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
llm_ghost_fake_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = fake_request;
  iface->request_finish = fake_request_finish;
}

LlmGhostBackend *
llm_ghost_fake_backend_new (const char *canned_response)
{
  LlmGhostFakeBackend *self = g_object_new (LLM_GHOST_TYPE_FAKE_BACKEND, NULL);
  self->canned = g_strdup (canned_response);
  return LLM_GHOST_BACKEND (self);
}
