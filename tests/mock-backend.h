#pragma once

#include "llmghost-backend.h"

G_BEGIN_DECLS

#define MOCK_TYPE_BACKEND (mock_backend_get_type ())
G_DECLARE_FINAL_TYPE (MockBackend, mock_backend, MOCK, BACKEND, GObject)

/* Deferred-by-default test double for LlmGhostBackend. Requests are parked
 * until mock_backend_complete_pending() is called, so tests control timing.
 * A request whose GCancellable fires is counted and completes with
 * G_IO_ERROR_CANCELLED instead. */
LlmGhostBackend *mock_backend_new            (void);
void             mock_backend_set_response   (MockBackend *self, const char *text);
guint            mock_backend_request_count  (MockBackend *self);
guint            mock_backend_cancel_count   (MockBackend *self);
void             mock_backend_complete_pending (MockBackend *self);

G_END_DECLS
