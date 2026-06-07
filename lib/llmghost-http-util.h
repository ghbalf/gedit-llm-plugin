#pragma once

/* Internal (NOT installed) shared HTTP helper for the HTTP-based backends.
 * POSTs a JSON body and hands back the parsed JSON response. */

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* POST @json_body to @url with arbitrary request @headers (each member of the
 * JsonObject is sent as "name: value"; non-string values are skipped with a
 * warning). Sets "Content-Type: application/json" unless @headers supplies its
 * own "Content-Type". Takes ownership of @json_body (frees it). @session,
 * @cancellable, @headers are borrowed. Finish with the same
 * _llm_ghost_http_post_json_finish() below. */
void       _llm_ghost_http_post_json_headers_async (SoupSession         *session,
                                                    const char          *url,
                                                    JsonObject          *headers,
                                                    char                *json_body,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

/* POST @json_body to @url. Attaches "Authorization: Bearer <bearer>" iff
 * @bearer is non-NULL and non-empty. Takes ownership of @json_body (frees it).
 * @session and @cancellable are borrowed. */
void       _llm_ghost_http_post_json_async  (SoupSession         *session,
                                             const char          *url,
                                             const char          *bearer,
                                             char                *json_body,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);

/* Returns the parsed root JsonNode (caller owns via json_node_unref), or
 * NULL + @error on transport failure, non-2xx HTTP (message carries the
 * status code and a body snippet), or malformed JSON. */
JsonNode * _llm_ghost_http_post_json_finish (GAsyncResult        *result,
                                             GError             **error);

/* Parse a JSON document @text into a newly-allocated root node (caller owns
 * via json_node_unref), or NULL if @text is empty or not valid JSON. */
JsonNode * _llm_ghost_http_parse_json (const char *text);

/* Called for each complete SSE "data:" payload, in order, on the main context. */
typedef void (*LlmGhostSseEventFn) (const char *payload, gpointer user_data);

/* POST @json_body to @url and stream the text/event-stream response: each
 * complete event payload is delivered to @on_event(@event_data). @headers
 * (string->string, nullable) are applied as for the non-streaming call; supply
 * auth here. Takes ownership of @json_body. @session, @cancellable, @headers,
 * @event_data are borrowed (read synchronously before return / during reads).
 * Finish with _llm_ghost_http_post_json_stream_finish(). */
void       _llm_ghost_http_post_json_stream_async  (SoupSession         *session,
                                                    const char          *url,
                                                    JsonObject          *headers,
                                                    char                *json_body,
                                                    LlmGhostSseEventFn   on_event,
                                                    gpointer             event_data,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

/* TRUE when the stream completed (EOF after a 2xx). FALSE + @error on transport
 * failure, non-2xx HTTP (message carries the status + body snippet), or cancel. */
gboolean   _llm_ghost_http_post_json_stream_finish (GAsyncResult        *result,
                                                    GError             **error);

G_END_DECLS
