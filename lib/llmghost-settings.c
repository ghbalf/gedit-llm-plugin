#define G_LOG_DOMAIN "llmghost-settings"

#include "llmghost-settings.h"
#include "llmghost-settings-internal.h"

#include <string.h>
#include <gio/gio.h>

/* Built-in default config, written to disk on first run and used as the
 * fallback when the user's file is missing or malformed. */
static const char DEFAULT_SETTINGS_JSON[] =
  "{\n"
  "  \"_help\": \"Edit this file in gedit; completions reload automatically. ${VARS} expand from the environment.\",\n"
  "  \"backend\": \"ollama\",\n"
  "  \"debounce_ms\": 80,\n"
  "  \"backends\": {\n"
  "    \"ollama\":  { \"host\": \"spark-2448\", \"port\": 11434,\n"
  "                 \"model\": \"qwen3-coder-next:latest\", \"tokens\": \"Qwen\" },\n"
  "    \"openai\":  { \"base_url\": \"https://api.openai.com/v1\", \"model\": \"gpt-4o-mini\",\n"
  "                 \"api_key\": \"${OPENAI_API_KEY}\", \"mode\": \"chat\" },\n"
  "    \"mistral\": { \"base_url\": \"https://codestral.mistral.ai/v1\",\n"
  "                 \"model\": \"codestral-latest\", \"api_key\": \"${MISTRAL_API_KEY}\" }\n"
  "  }\n"
  "}\n";

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

struct _LlmGhostSettings
{
  GObject       parent_instance;
  char         *path;        /* backing file, or NULL (string-only) */
  JsonObject   *root;        /* interpolated config (owned) */
  GFileMonitor *monitor;     /* NULL when no backing file */
};

G_DEFINE_TYPE (LlmGhostSettings, llm_ghost_settings, G_TYPE_OBJECT)

/* ---- ${ENV} interpolation ---------------------------------------------- */

char *
_llm_ghost_settings_interpolate (const char *in)
{
  if (in == NULL)
    return g_strdup ("");

  GString *out = g_string_new (NULL);
  const char *p = in;
  while (*p != '\0')
    {
      if (p[0] == '$' && p[1] == '{')
        {
          const char *end = strchr (p + 2, '}');
          if (end != NULL)
            {
              char *name = g_strndup (p + 2, (gsize) (end - (p + 2)));
              const char *val = g_getenv (name);
              if (val == NULL)
                {
                  g_warning ("environment variable ${%s} is not set; using \"\"", name);
                  val = "";
                }
              g_string_append (out, val);
              g_free (name);
              p = end + 1;
              continue;
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

/* Replace every string value in @obj (recursing into nested objects) with its
 * interpolation. Our config has no string arrays, so arrays are left alone. */
static void
interpolate_object (JsonObject *obj)
{
  GList *members = json_object_get_members (obj);   /* snapshot of keys */
  for (GList *l = members; l != NULL; l = l->next)
    {
      const char *key = l->data;
      JsonNode *child = json_object_get_member (obj, key);
      if (JSON_NODE_HOLDS_OBJECT (child))
        interpolate_object (json_node_get_object (child));
      else if (JSON_NODE_HOLDS_VALUE (child) &&
               json_node_get_value_type (child) == G_TYPE_STRING)
        {
          char *interp = _llm_ghost_settings_interpolate (json_node_get_string (child));
          json_object_set_string_member (obj, key, interp);
          g_free (interp);
        }
    }
  g_list_free (members);
}

/* ---- parsing ----------------------------------------------------------- */

/* Parse @json and interpolate every string. Returns an owned JsonObject, or
 * NULL when the text is not a JSON object. */
static JsonObject *
parse_and_interpolate (const char *json)
{
  JsonParser *parser = json_parser_new ();
  GError *error = NULL;
  if (!json_parser_load_from_data (parser, json, -1, &error))
    {
      g_warning ("parse error: %s", error->message);
      g_clear_error (&error);
      g_object_unref (parser);
      return NULL;
    }

  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_warning ("top-level value is not an object");
      g_object_unref (parser);
      return NULL;
    }

  JsonObject *obj = json_object_ref (json_node_get_object (root));
  g_object_unref (parser);                 /* cache outlives the parser */
  interpolate_object (obj);
  return obj;
}

/* Built-in defaults; always valid by construction. */
static JsonObject *
default_object (void)
{
  JsonObject *obj = parse_and_interpolate (DEFAULT_SETTINGS_JSON);
  g_assert (obj != NULL);
  return obj;
}

/* Swap in a new cached root, dropping the old one. Takes ownership of @obj. */
static void
set_root (LlmGhostSettings *self, JsonObject *obj)
{
  if (self->root != NULL)
    json_object_unref (self->root);
  self->root = obj;
}

/* ---- construction (string seam) ---------------------------------------- */

LlmGhostSettings *
_llm_ghost_settings_new_from_string (const char *json)
{
  LlmGhostSettings *self = g_object_new (LLM_GHOST_TYPE_SETTINGS, NULL);
  JsonObject *obj = parse_and_interpolate (json);
  set_root (self, obj != NULL ? obj : default_object ());
  return self;
}

char *
llm_ghost_settings_default_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "llmghost", "settings.json", NULL);
}

/* ---- accessors --------------------------------------------------------- */

const char *
llm_ghost_settings_get_active_backend (LlmGhostSettings *self)
{
  JsonNode *n = json_object_get_member (self->root, "backend");
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n) &&
      json_node_get_value_type (n) == G_TYPE_STRING)
    {
      const char *s = json_node_get_string (n);
      if (s != NULL && *s != '\0')
        return s;
    }
  return "ollama";
}

gboolean
llm_ghost_settings_get_debounce_ms (LlmGhostSettings *self, guint *out_ms)
{
  JsonNode *n = json_object_get_member (self->root, "debounce_ms");
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n))
    {
      GType t = json_node_get_value_type (n);
      if (t == G_TYPE_INT64 || t == G_TYPE_DOUBLE)
        {
          gint64 v = (t == G_TYPE_DOUBLE) ? (gint64) json_node_get_double (n)
                                          : json_node_get_int (n);
          if (v > 0)
            {
              if (out_ms != NULL)
                *out_ms = (guint) v;
              return TRUE;
            }
        }
    }
  return FALSE;
}

JsonObject *
llm_ghost_settings_get_backend_params (LlmGhostSettings *self, const char *name)
{
  JsonNode *bn = json_object_get_member (self->root, "backends");
  if (bn == NULL || !JSON_NODE_HOLDS_OBJECT (bn))
    return NULL;
  JsonObject *backends = json_node_get_object (bn);
  JsonNode *pn = json_object_get_member (backends, name);
  if (pn == NULL || !JSON_NODE_HOLDS_OBJECT (pn))
    return NULL;
  return json_node_get_object (pn);
}

/* ---- GObject lifecycle ------------------------------------------------- */

static void
llm_ghost_settings_finalize (GObject *object)
{
  LlmGhostSettings *self = LLM_GHOST_SETTINGS (object);
  if (self->monitor != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->monitor, self);
      g_clear_object (&self->monitor);
    }
  g_clear_pointer (&self->root, json_object_unref);
  g_clear_pointer (&self->path, g_free);
  G_OBJECT_CLASS (llm_ghost_settings_parent_class)->finalize (object);
}

static void
llm_ghost_settings_class_init (LlmGhostSettingsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = llm_ghost_settings_finalize;

  /* Emitted after a live reload swaps in a new, valid config. */
  signals[SIG_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
llm_ghost_settings_init (LlmGhostSettings *self)
{
  (void) self;
}
