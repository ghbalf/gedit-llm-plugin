#include "mock-backend.h"

typedef struct {
  GTask        *task;
  GCancellable *cancellable;   /* reffed, or NULL */
  gulong        cancel_id;     /* g_cancellable_connect id, or 0 */
  MockBackend  *self;          /* unowned backref */
} Pending;

struct _MockBackend
{
  GObject  parent_instance;
  char    *response;
  guint    request_count;
  guint    cancel_count;
  GList   *pending;            /* Pending* */
};

static void mock_backend_iface_init (LlmGhostBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE (MockBackend, mock_backend, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (LLM_GHOST_TYPE_BACKEND,
                                                mock_backend_iface_init))

static void
pending_free (Pending *p)
{
  g_clear_object (&p->cancellable);
  g_free (p);
}

/* Fires (synchronously, from g_cancellable_cancel) when a parked request's
 * cancellable is cancelled. Do NOT call g_cancellable_disconnect here — that
 * deadlocks when invoked from within the cancelled handler. */
static void
on_cancelled (GCancellable *cancellable, gpointer user_data)
{
  (void) cancellable;
  Pending *p = user_data;
  MockBackend *self = p->self;

  GList *link = g_list_find (self->pending, p);
  if (link == NULL)
    return;   /* already completed */

  self->pending = g_list_delete_link (self->pending, link);
  self->cancel_count++;
  p->cancel_id = 0;   /* fired; nothing left to disconnect */

  g_task_return_new_error (p->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                           "mock: cancelled");
  g_object_unref (p->task);
  pending_free (p);
}

static void
mock_request (LlmGhostBackend     *backend,
              const char          *prefix,
              const char          *suffix,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
  (void) prefix;
  (void) suffix;
  MockBackend *self = MOCK_BACKEND (backend);
  self->request_count++;

  /* Precondition: a non-NULL cancellable must not already be cancelled here.
   * g_cancellable_connect() fires on_cancelled() synchronously for an
   * already-cancelled cancellable — before p is appended below — so the
   * handler wouldn't find p in `pending` and the task would leak unresolved.
   * The controller always passes a freshly-created cancellable, so this holds.
   * (Appending before connecting is NOT a fix: the synchronous handler frees
   * p, and the subsequent `p->cancel_id =` would then write to freed memory.) */
  Pending *p = g_new0 (Pending, 1);
  p->self = self;
  p->task = g_task_new (self, cancellable, callback, user_data);
  if (cancellable != NULL)
    {
      p->cancellable = g_object_ref (cancellable);
      p->cancel_id = g_cancellable_connect (cancellable,
                                            G_CALLBACK (on_cancelled), p, NULL);
    }
  self->pending = g_list_append (self->pending, p);
}

static char *
mock_request_finish (LlmGhostBackend  *backend,
                     GAsyncResult     *result,
                     GError          **error)
{
  (void) backend;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
mock_backend_iface_init (LlmGhostBackendInterface *iface)
{
  iface->request        = mock_request;
  iface->request_finish = mock_request_finish;
}

void
mock_backend_set_response (MockBackend *self, const char *text)
{
  g_clear_pointer (&self->response, g_free);
  self->response = g_strdup (text);
}

guint
mock_backend_request_count (MockBackend *self)
{
  return self->request_count;
}

guint
mock_backend_cancel_count (MockBackend *self)
{
  return self->cancel_count;
}

void
mock_backend_complete_pending (MockBackend *self)
{
  GList *snapshot = self->pending;
  self->pending = NULL;

  for (GList *l = snapshot; l != NULL; l = l->next)
    {
      Pending *p = l->data;
      if (p->cancel_id != 0 && p->cancellable != NULL)
        g_cancellable_disconnect (p->cancellable, p->cancel_id);
      g_task_return_pointer (p->task,
                             g_strdup (self->response ? self->response : "MOCK"),
                             g_free);
      g_object_unref (p->task);
      pending_free (p);
    }
  g_list_free (snapshot);
}

static void
mock_backend_finalize (GObject *object)
{
  MockBackend *self = MOCK_BACKEND (object);
  for (GList *l = self->pending; l != NULL; l = l->next)
    {
      Pending *p = l->data;
      if (p->cancel_id != 0 && p->cancellable != NULL)
        g_cancellable_disconnect (p->cancellable, p->cancel_id);
      g_task_return_new_error (p->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                               "mock: backend finalized");
      g_object_unref (p->task);
      pending_free (p);
    }
  g_clear_pointer (&self->pending, g_list_free);
  g_clear_pointer (&self->response, g_free);
  G_OBJECT_CLASS (mock_backend_parent_class)->finalize (object);
}

static void
mock_backend_class_init (MockBackendClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = mock_backend_finalize;
}

static void
mock_backend_init (MockBackend *self)
{
  (void) self;
}

LlmGhostBackend *
mock_backend_new (void)
{
  return LLM_GHOST_BACKEND (g_object_new (MOCK_TYPE_BACKEND, NULL));
}
