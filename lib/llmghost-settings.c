#define G_LOG_DOMAIN "llmghost-settings"

#include "llmghost-settings.h"
#include "llmghost-settings-internal.h"
#include "llmghost-secret-store.h"

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

static char *
default_secret_lookup (const char *name)
{
  return llm_ghost_secret_lookup (name, NULL);   /* NULL on any failure */
}

static LlmGhostSecretLookupFn secret_lookup_fn = default_secret_lookup;

void
_llm_ghost_settings_set_secret_lookup_for_testing (LlmGhostSecretLookupFn fn)
{
  secret_lookup_fn = fn != NULL ? fn : default_secret_lookup;
}

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
              if (g_str_has_prefix (name, "secret:"))
                {
                  const char *sname = name + strlen ("secret:");
                  char *val = secret_lookup_fn (sname);
                  if (val == NULL)
                    g_warning ("secret ${secret:%s} is not available; using \"\"", sname);
                  g_string_append (out, val != NULL ? val : "");
                  g_free (val);
                }
              else
                {
                  const char *envv = g_getenv (name);
                  if (envv == NULL)
                    {
                      g_warning ("environment variable ${%s} is not set; using \"\"", name);
                      envv = "";
                    }
                  g_string_append (out, envv);
                }
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

/* Replace every string value reachable from @node with its interpolation,
 * recursing through objects AND arrays (the generic backend's request_template
 * nests strings inside arrays). Strings are mutated in place via
 * json_node_set_string, which works uniformly for object members and array
 * elements (json-glib has no array-element string setter) — mirroring the
 * request-time substitute_node in the generic backend. */
static void
interpolate_node (JsonNode *node)
{
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      JsonObject *obj = json_node_get_object (node);
      GList *members = json_object_get_members (obj);   /* snapshot of keys */
      for (GList *l = members; l != NULL; l = l->next)
        interpolate_node (json_object_get_member (obj, l->data));
      g_list_free (members);
    }
  else if (JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *arr = json_node_get_array (node);
      guint n = json_array_get_length (arr);
      for (guint i = 0; i < n; i++)
        interpolate_node (json_array_get_element (arr, i));
    }
  else if (JSON_NODE_HOLDS_VALUE (node) &&
           json_node_get_value_type (node) == G_TYPE_STRING)
    {
      char *interp = _llm_ghost_settings_interpolate (json_node_get_string (node));
      json_node_set_string (node, interp);
      g_free (interp);
    }
}

static void
interpolate_object (JsonObject *obj)
{
  GList *members = json_object_get_members (obj);   /* snapshot of keys */
  for (GList *l = members; l != NULL; l = l->next)
    interpolate_node (json_object_get_member (obj, l->data));
  g_list_free (members);
}

/* ---- ${secret:NAME} reference collection (for the prefs dialog) --------- */

static void
collect_refs_from_string (const char *s, GHashTable *set)
{
  const char *p = s;
  while ((p = strstr (p, "${secret:")) != NULL)
    {
      const char *start = p + strlen ("${secret:");
      const char *end = strchr (start, '}');
      if (end == NULL)
        break;
      char *name = g_strndup (start, (gsize) (end - start));
      if (*name != '\0' && !g_hash_table_contains (set, name))
        g_hash_table_add (set, name);     /* set takes ownership */
      else
        g_free (name);
      p = end + 1;
    }
}

static void
collect_refs_from_node (JsonNode *node, GHashTable *set)
{
  if (JSON_NODE_HOLDS_OBJECT (node))
    {
      JsonObject *obj = json_node_get_object (node);
      GList *members = json_object_get_members (obj);
      for (GList *l = members; l != NULL; l = l->next)
        collect_refs_from_node (json_object_get_member (obj, l->data), set);
      g_list_free (members);
    }
  else if (JSON_NODE_HOLDS_ARRAY (node))
    {
      JsonArray *arr = json_node_get_array (node);
      guint n = json_array_get_length (arr);
      for (guint i = 0; i < n; i++)
        collect_refs_from_node (json_array_get_element (arr, i), set);
    }
  else if (JSON_NODE_HOLDS_VALUE (node) &&
           json_node_get_value_type (node) == G_TYPE_STRING)
    {
      collect_refs_from_string (json_node_get_string (node), set);
    }
}

char **
_llm_ghost_settings_collect_secret_refs (JsonObject *root)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (root != NULL)
    {
      GList *members = json_object_get_members (root);
      for (GList *l = members; l != NULL; l = l->next)
        collect_refs_from_node (json_object_get_member (root, l->data), set);
      g_list_free (members);
    }

  char **out = g_new0 (char *, g_hash_table_size (set) + 1);
  GHashTableIter it;
  gpointer key;
  guint i = 0;
  g_hash_table_iter_init (&it, set);
  while (g_hash_table_iter_next (&it, &key, NULL))
    out[i++] = g_strdup ((char *) key);
  out[i] = NULL;

  g_hash_table_destroy (set);             /* frees the owned key strings */
  return out;
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

gboolean
llm_ghost_settings_touch (const char *path, GError **error)
{
  g_return_val_if_fail (path != NULL, FALSE);
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, error))
    return FALSE;
  gboolean ok = g_file_set_contents (path, data, (gssize) len, error);
  g_free (data);
  return ok;
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

/* ---- file loading + live reload ---------------------------------------- */

static void
ensure_file_exists (const char *path)
{
  if (g_file_test (path, G_FILE_TEST_EXISTS))
    return;
  char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_free (dir);

  GError *error = NULL;
  if (!g_file_set_contents (path, DEFAULT_SETTINGS_JSON, -1, &error))
    {
      g_warning ("could not write default %s: %s", path, error->message);
      g_clear_error (&error);
    }
}

/* Read self->path into the cache. On read or parse failure, fall back to
 * built-in defaults WITHOUT overwriting the user's file. */
static void
load_from_file (LlmGhostSettings *self)
{
  char *data = NULL;
  GError *error = NULL;
  if (!g_file_get_contents (self->path, &data, NULL, &error))
    {
      g_warning ("could not read %s: %s; using defaults", self->path, error->message);
      g_clear_error (&error);
      set_root (self, default_object ());
      return;
    }
  JsonObject *obj = parse_and_interpolate (data);
  g_free (data);
  set_root (self, obj != NULL ? obj : default_object ());
}

void
_llm_ghost_settings_reload (LlmGhostSettings *self)
{
  if (self->path == NULL)
    return;

  char *data = NULL;
  GError *error = NULL;
  if (!g_file_get_contents (self->path, &data, NULL, &error))
    {
      g_warning ("reload could not read %s: %s; keeping last config",
                 self->path, error->message);
      g_clear_error (&error);
      return;
    }
  JsonObject *obj = parse_and_interpolate (data);
  g_free (data);
  if (obj == NULL)
    {
      g_warning ("reload parse failed; keeping last config");
      return;                                    /* keep last-good, no signal */
    }
  set_root (self, obj);
  g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static void
on_file_changed (GFileMonitor *mon, GFile *file, GFile *other,
                 GFileMonitorEvent ev, gpointer user_data)
{
  (void) mon; (void) file; (void) other;
  if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
      ev == G_FILE_MONITOR_EVENT_CREATED)
    _llm_ghost_settings_reload (LLM_GHOST_SETTINGS (user_data));
}

LlmGhostSettings *
llm_ghost_settings_new (const char *path)
{
  LlmGhostSettings *self = g_object_new (LLM_GHOST_TYPE_SETTINGS, NULL);
  self->path = path != NULL ? g_strdup (path) : llm_ghost_settings_default_path ();

  ensure_file_exists (self->path);
  load_from_file (self);

  GFile *file = g_file_new_for_path (self->path);
  GError *error = NULL;
  self->monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
  if (self->monitor != NULL)
    g_signal_connect (self->monitor, "changed", G_CALLBACK (on_file_changed), self);
  else
    {
      g_warning ("could not watch %s: %s", self->path, error->message);
      g_clear_error (&error);
    }
  g_object_unref (file);
  return self;
}

/* ---- GObject lifecycle ------------------------------------------------- */

static void
llm_ghost_settings_finalize (GObject *object)
{
  LlmGhostSettings *self = LLM_GHOST_SETTINGS (object);
  if (self->monitor != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->monitor, self);
      g_file_monitor_cancel (self->monitor);   /* release the kernel watch */
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
