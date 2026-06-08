#pragma once

/* Testing-only internal API for the generic backend. NOT installed. Pure
 * template-substitution and response-path extraction for direct unit testing. */

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "llmghost-generic-backend.h"

G_BEGIN_DECLS

/* Deep-copy @template and replace the tokens {{prefix}}, {{suffix}}, {{model}}
 * inside every string value (objects and arrays, recursively). Each string is
 * scanned once left-to-right, so substituted content is never re-scanned (a
 * prefix containing "{{suffix}}" is inserted verbatim). Unknown {{tokens}} are
 * left as-is. NULL placeholder values are treated as "". Returns a
 * newly-allocated serialized JSON string. @template is not modified. */
char *_llm_ghost_generic_build_body (JsonObject *template,
                                     const char *prefix,
                                     const char *suffix,
                                     const char *model);

/* Walk a dotted @path through @root. An all-digits segment indexes an array;
 * any other segment selects an object member. Returns the located string
 * (newly-allocated), or NULL + @error (G_IO_ERROR_FAILED, message naming the
 * failing segment) when the path does not resolve to a string. */
char *_llm_ghost_generic_extract (JsonNode  *root,
                                  const char *path,
                                  GError    **error);

/* Like _llm_ghost_generic_build_body but also sets the top-level @stream_field
 * member of the body object to @stream_value before serializing. If
 * @stream_field is NULL or "", the template is left untouched. */
char *_llm_ghost_generic_build_body_with_stream (JsonObject *template,
                                                 const char *prefix,
                                                 const char *suffix,
                                                 const char *model,
                                                 const char *stream_field,
                                                 gboolean    stream_value);

/* Like _llm_ghost_generic_extract but returns "" (never an error) when @path
 * is absent / not a string in @event — for per-event SSE deltas. */
char *_llm_ghost_generic_extract_delta (JsonNode   *event,
                                        const char *path);

/* Configure streaming. @stream gates it; streaming is active only when
 * @stream is TRUE and @stream_path is non-empty. @done_marker (NULL/"" ->
 * "[DONE]") is the event payload to skip. @stream_field (NULL -> keep default
 * "stream"; explicit "" -> disable body mutation) names the body member set to
 * @stream's wire value. Copies the strings. */
void  _llm_ghost_generic_backend_set_streaming (LlmGhostGenericBackend *self,
                                                gboolean    stream,
                                                const char *stream_path,
                                                const char *done_marker,
                                                const char *stream_field);

G_END_DECLS
