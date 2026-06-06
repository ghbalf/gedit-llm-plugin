#pragma once

#include "llmghost-backend.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define LLM_GHOST_TYPE_GENERIC_BACKEND (llm_ghost_generic_backend_get_type())
G_DECLARE_FINAL_TYPE (LlmGhostGenericBackend, llm_ghost_generic_backend,
                      LLM_GHOST, GENERIC_BACKEND, GObject)

/**
 * llm_ghost_generic_backend_new:
 * @url:              endpoint URL (may already contain an interpolated key).
 * @headers:          request headers (string→string), or %NULL. Refs it.
 * @model:            value substituted for {{model}}, or %NULL.
 * @request_template: JSON body template with {{prefix}}/{{suffix}}/{{model}}. Refs it.
 * @response_path:    dotted path to the completion string in the response.
 *
 * A config-driven backend for non-OpenAI-shaped JSON-over-POST APIs. Holds an
 * owning ref on @headers and @request_template so it is independent of the
 * settings object's lifetime (and a live reload). Missing required fields are
 * logged; requests then fail gracefully.
 */
LlmGhostBackend *llm_ghost_generic_backend_new (const char *url,
                                                JsonObject *headers,
                                                const char *model,
                                                JsonObject *request_template,
                                                const char *response_path);

G_END_DECLS
