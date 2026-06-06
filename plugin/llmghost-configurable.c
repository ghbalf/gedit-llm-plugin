/* Preferences entry point: a PeasGtkConfigurable that shows the settings-file
 * path and an "Open settings.json" button. The file is the main UI; the one
 * field-by-field widget is the Secrets section, which manages keyring keys
 * referenced as ${secret:NAME} in settings.json. */

#include "llmghost-configurable.h"

#include <gtk/gtk.h>
#include <libpeas-gtk/peas-gtk.h>
#include <json-glib/json-glib.h>

#include "llmghost.h"   /* llm_ghost_settings_new / _default_path / _touch */
#include "llmghost-settings-internal.h"   /* _llm_ghost_settings_collect_secret_refs */
#include "llmghost-secret-store.h"          /* llm_ghost_secret_lookup/store/clear */

struct _LlmghostConfigurable
{
  PeasExtensionBase parent_instance;
};

static void configurable_iface_init (PeasGtkConfigurableInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (
  LlmghostConfigurable,
  llmghost_configurable,
  PEAS_TYPE_EXTENSION_BASE,
  0,
  G_IMPLEMENT_INTERFACE_DYNAMIC (PEAS_GTK_TYPE_CONFIGURABLE,
                                 configurable_iface_init))

/* GClosureNotify-compatible wrapper for g_free, avoiding -Wcast-function-type. */
static void
closure_free (gpointer data, GClosure *closure)
{
  (void) closure;
  g_free (data);
}

static void
on_open_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  const char *path = user_data;

  /* Make sure the file exists (writes the default on first use) before we
   * hand it to the desktop. */
  LlmGhostSettings *s = llm_ghost_settings_new (path);
  g_object_unref (s);

  char *uri = g_filename_to_uri (path, NULL, NULL);
  if (uri != NULL)
    {
      GError *error = NULL;
      if (!gtk_show_uri_on_window (NULL, uri, GDK_CURRENT_TIME, &error))
        {
          g_warning ("could not open %s: %s", uri, error->message);
          g_clear_error (&error);
        }
      g_free (uri);
    }
}

/* Parse the settings file WITHOUT interpolation (so ${secret:NAME} survives)
 * into an owned JsonObject, or NULL. */
static JsonObject *
load_raw_settings (const char *path)
{
  JsonParser *parser = json_parser_new ();
  JsonObject *obj = NULL;
  if (json_parser_load_from_file (parser, path, NULL))
    {
      JsonNode *root = json_parser_get_root (parser);
      if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        obj = json_object_ref (json_node_get_object (root));
    }
  g_object_unref (parser);
  return obj;
}

typedef struct
{
  char     *name;     /* secret name (owned) */
  char     *path;     /* settings path (owned) */
  GtkEntry *entry;    /* not owned (lives in the widget tree) */
  GtkLabel *status;   /* not owned */
} SecretRow;

static void
secret_row_destroy (gpointer data)
{
  SecretRow *r = data;
  g_free (r->name);
  g_free (r->path);
  g_free (r);
}

static void
secret_row_update_status (SecretRow *r)
{
  char *v = llm_ghost_secret_lookup (r->name, NULL);
  gtk_label_set_text (r->status, v != NULL ? "stored" : "not stored");
  g_free (v);
}

static void
on_secret_store_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SecretRow *r = user_data;
  const char *val = gtk_entry_get_text (r->entry);
  if (*val == '\0')
    {
      gtk_label_set_text (r->status, "enter a value first");
      return;
    }

  GError *error = NULL;
  if (!llm_ghost_secret_store (r->name, val, &error))
    {
      gtk_label_set_text (r->status, error != NULL ? error->message : "store failed");
      g_clear_error (&error);
      return;
    }

  gtk_entry_set_text (r->entry, "");                 /* don't keep it on screen */
  llm_ghost_settings_touch (r->path, NULL);          /* apply without restart */
  secret_row_update_status (r);
}

static void
on_secret_clear_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SecretRow *r = user_data;
  GError *error = NULL;
  if (!llm_ghost_secret_clear (r->name, &error))
    {
      gtk_label_set_text (r->status, error != NULL ? error->message : "clear failed");
      g_clear_error (&error);
      return;
    }
  llm_ghost_settings_touch (r->path, NULL);
  secret_row_update_status (r);
}

/* Build the "Secrets" section for the given settings @path. */
static GtkWidget *
build_secrets_section (const char *path)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  GtkWidget *heading = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (heading), "<b>Secrets (keyring)</b>");
  gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
  gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

  JsonObject *raw = load_raw_settings (path);
  char **names = _llm_ghost_settings_collect_secret_refs (raw);
  if (raw != NULL)
    json_object_unref (raw);

  if (names[0] == NULL)
    {
      GtkWidget *hint = gtk_label_new (
        "No ${secret:NAME} references in settings.json. Add one — e.g. "
        "\"api_key\": \"${secret:openai}\" — to manage that key here.");
      gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
      gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
      gtk_box_pack_start (GTK_BOX (box), hint, FALSE, FALSE, 0);
      g_strfreev (names);
      return box;
    }

  for (int i = 0; names[i] != NULL; i++)
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

      GtkWidget *label = gtk_label_new (names[i]);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_size_request (label, 120, -1);

      GtkWidget *entry = gtk_entry_new ();
      gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
      gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "enter key…");
      gtk_widget_set_hexpand (entry, TRUE);

      GtkWidget *store_btn = gtk_button_new_with_label ("Store");
      GtkWidget *clear_btn = gtk_button_new_with_label ("Clear");
      GtkWidget *status    = gtk_label_new (NULL);

      SecretRow *r = g_new0 (SecretRow, 1);
      r->name   = g_strdup (names[i]);
      r->path   = g_strdup (path);
      r->entry  = GTK_ENTRY (entry);
      r->status = GTK_LABEL (status);
      /* The row box owns the SecretRow; both buttons borrow it. */
      g_object_set_data_full (G_OBJECT (row), "secret-row", r, secret_row_destroy);

      g_signal_connect (store_btn, "clicked", G_CALLBACK (on_secret_store_clicked), r);
      g_signal_connect (clear_btn, "clicked", G_CALLBACK (on_secret_clear_clicked), r);

      gtk_box_pack_start (GTK_BOX (row), label,     FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (row), entry,     TRUE,  TRUE,  0);
      gtk_box_pack_start (GTK_BOX (row), store_btn, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (row), clear_btn, FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (row), status,    FALSE, FALSE, 0);
      gtk_box_pack_start (GTK_BOX (box), row, FALSE, FALSE, 0);

      secret_row_update_status (r);
    }

  g_strfreev (names);
  return box;
}

static GtkWidget *
configurable_create_widget (PeasGtkConfigurable *configurable)
{
  (void) configurable;
  char *path = llm_ghost_settings_default_path ();   /* owned by the button below */

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);

  /* Escape the path: an XDG dir containing &, < or > would break the markup. */
  char *escaped = g_markup_escape_text (path, -1);
  char *markup = g_strdup_printf ("Settings file:\n<tt>%s</tt>", escaped);
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  g_free (markup);
  g_free (escaped);

  GtkWidget *button = gtk_button_new_with_label ("Open settings.json");
  /* The closure owns `path` and frees it when the button is destroyed. */
  g_signal_connect_data (button, "clicked", G_CALLBACK (on_open_clicked),
                         path, closure_free, 0);

  gtk_box_pack_start (GTK_BOX (box), label,  FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box),
                      gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), build_secrets_section (path), FALSE, FALSE, 0);
  gtk_widget_show_all (box);
  return box;
}

static void
configurable_iface_init (PeasGtkConfigurableInterface *iface)
{
  iface->create_configure_widget = configurable_create_widget;
}

static void
llmghost_configurable_init (LlmghostConfigurable *self)
{
  (void) self;
}

static void
llmghost_configurable_class_init (LlmghostConfigurableClass *klass)
{
  (void) klass;
}

static void
llmghost_configurable_class_finalize (LlmghostConfigurableClass *klass)
{
  (void) klass;
}

void
llmghost_configurable_register (PeasObjectModule *module)
{
  llmghost_configurable_register_type (G_TYPE_MODULE (module));
  peas_object_module_register_extension_type (module,
                                              PEAS_GTK_TYPE_CONFIGURABLE,
                                              LLMGHOST_TYPE_CONFIGURABLE);
}
