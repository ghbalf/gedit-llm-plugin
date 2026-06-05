#pragma once

/* Testing-only internal API for the generic backend. NOT installed. Pure
 * template-substitution and response-path extraction for direct unit testing. */

#include <glib.h>
#include <json-glib/json-glib.h>

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

G_END_DECLS
