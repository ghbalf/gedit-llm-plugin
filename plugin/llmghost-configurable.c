/* Preferences entry point: a PeasGtkConfigurable that shows the settings-file
 * path and an "Open settings.json" button. The file IS the UI in v1 — there is
 * no field-by-field widget. */

#include "llmghost-configurable.h"

#include <gtk/gtk.h>
#include <libpeas-gtk/peas-gtk.h>

#include "llmghost.h"   /* llm_ghost_settings_new / _default_path */

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
