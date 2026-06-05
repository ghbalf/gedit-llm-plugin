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

G_END_DECLS
