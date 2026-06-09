/* gedit plugin layer — wires LlmGhostController onto every view in a
 * GeditWindow. ~150 lines of libpeas glue; all completion logic lives in
 * libllmghost. See ../NOTES.md "Phase 3" for the design rationale. */

#include "llmghost-plugin.h"
#include "llmghost-configurable.h"

#include <gedit/gedit-window.h>
#include <gedit/gedit-window-activatable.h>
#include <gedit/gedit-tab.h>
#include <gedit/gedit-view.h>

#include "llmghost.h"

#define CONTROLLER_DATA_KEY "llmghost-controller"

enum
{
  PROP_0,
  PROP_WINDOW,
};

struct _LlmghostPlugin
{
  PeasExtensionBase  parent_instance;

  GeditWindow       *window;          /* injected by libpeas via PROP_WINDOW */
  LlmGhostSettings  *settings;        /* owns the config file + monitor */
  LlmGhostBackend   *backend;         /* rebuilt from settings on "changed" */

  gulong             h_tab_added;
  gulong             h_tab_removed;
  gulong             h_settings_changed;
};

static void llmghost_plugin_iface_init (GeditWindowActivatableInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (
  LlmghostPlugin,
  llmghost_plugin,
  PEAS_TYPE_EXTENSION_BASE,
  0,
  G_IMPLEMENT_INTERFACE_DYNAMIC (GEDIT_TYPE_WINDOW_ACTIVATABLE,
                                 llmghost_plugin_iface_init))

/* ---- per-view controller management ------------------------------------ */

static void
attach_controller (LlmghostPlugin *self, GeditView *view)
{
  if (g_object_get_data (G_OBJECT (view), CONTROLLER_DATA_KEY) != NULL)
    return;

  LlmGhostController *ctrl = llm_ghost_controller_new (
    GTK_TEXT_VIEW (view), self->backend);

  guint ms;
  if (llm_ghost_settings_get_debounce_ms (self->settings, &ms))
    llm_ghost_controller_set_debounce_ms (ctrl, ms);

  guint mlines;
  if (llm_ghost_settings_get_max_lines (self->settings, &mlines))
    llm_ghost_controller_set_max_lines (ctrl, mlines);

  /* Lifetime tied to the view: when the view is destroyed (tab close,
   * window close), the destroy notify drops the last ref on the
   * controller, which disconnects its signal handlers in finalize. */
  g_object_set_data_full (G_OBJECT (view), CONTROLLER_DATA_KEY,
                          ctrl, g_object_unref);
}

static void
detach_controller (GeditView *view)
{
  /* Setting the data to NULL fires the destroy notify (g_object_unref)
   * on the controller — same teardown path as view destruction. */
  g_object_set_data (G_OBJECT (view), CONTROLLER_DATA_KEY, NULL);
}

/* Tear down and rebuild every view's controller so each binds to the current
 * self->backend and debounce setting. Called after a settings reload swaps the
 * backend. Requires self->backend to be non-NULL (attach_controller reads it).
 * Any ghost text in progress is discarded — acceptable for a config change. */
static void
reattach_all (LlmghostPlugin *self)
{
  GList *views = gedit_window_get_views (self->window);
  for (GList *l = views; l != NULL; l = l->next)
    {
      detach_controller (GEDIT_VIEW (l->data));
      attach_controller (self, GEDIT_VIEW (l->data));
    }
  g_list_free (views);
}

/* ---- signal handlers --------------------------------------------------- */

static void
on_tab_added (GeditWindow *window, GeditTab *tab, gpointer user_data)
{
  (void) window;
  LlmghostPlugin *self = LLMGHOST_PLUGIN (user_data);
  attach_controller (self, gedit_tab_get_view (tab));
}

static void
on_tab_removed (GeditWindow *window, GeditTab *tab, gpointer user_data)
{
  /* The view is about to be destroyed; the destroy notify will tear down
   * the controller. Nothing to do here, but the signal connection is
   * kept to give us a hook if cleanup ever needs to be more eager. */
  (void) window; (void) tab; (void) user_data;
}

static void
on_settings_changed (LlmGhostSettings *settings, gpointer user_data)
{
  (void) settings;
  LlmghostPlugin *self = LLMGHOST_PLUGIN (user_data);

  /* Build the new backend, then swap every controller onto it. Dropping the
   * plugin's ref here does not free the old backend while a request is still
   * in flight: each controller holds its own ref and is cancelled+unref'd as
   * reattach_all detaches it, so the old backend outlives any pending callback. */
  g_clear_object (&self->backend);
  self->backend = llm_ghost_backend_new_from_settings (self->settings);
  reattach_all (self);
}

/* ---- GeditWindowActivatable virtuals ----------------------------------- */

static void
llmghost_plugin_activate (GeditWindowActivatable *activatable)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (activatable);

  /* One settings object (and one backend) serves every view in this window. */
  self->settings = llm_ghost_settings_new (NULL);
  self->backend  = llm_ghost_backend_new_from_settings (self->settings);

  GList *views = gedit_window_get_views (self->window);
  for (GList *l = views; l != NULL; l = l->next)
    attach_controller (self, GEDIT_VIEW (l->data));
  g_list_free (views);

  self->h_tab_added = g_signal_connect (self->window, "tab-added",
                                        G_CALLBACK (on_tab_added), self);
  self->h_tab_removed = g_signal_connect (self->window, "tab-removed",
                                          G_CALLBACK (on_tab_removed), self);
  self->h_settings_changed = g_signal_connect (self->settings, "changed",
                                               G_CALLBACK (on_settings_changed), self);
}

static void
llmghost_plugin_deactivate (GeditWindowActivatable *activatable)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (activatable);

  if (self->h_settings_changed != 0)
    {
      g_signal_handler_disconnect (self->settings, self->h_settings_changed);
      self->h_settings_changed = 0;
    }
  if (self->h_tab_added != 0)
    {
      g_signal_handler_disconnect (self->window, self->h_tab_added);
      self->h_tab_added = 0;
    }
  if (self->h_tab_removed != 0)
    {
      g_signal_handler_disconnect (self->window, self->h_tab_removed);
      self->h_tab_removed = 0;
    }

  GList *views = gedit_window_get_views (self->window);
  for (GList *l = views; l != NULL; l = l->next)
    detach_controller (GEDIT_VIEW (l->data));
  g_list_free (views);

  g_clear_object (&self->backend);
  g_clear_object (&self->settings);   /* drops the GFileMonitor */
}

static void
llmghost_plugin_iface_init (GeditWindowActivatableInterface *iface)
{
  iface->activate   = llmghost_plugin_activate;
  iface->deactivate = llmghost_plugin_deactivate;
}

/* ---- GObject lifecycle -------------------------------------------------- */

static void
llmghost_plugin_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (object);
  switch (prop_id)
    {
    case PROP_WINDOW:
      self->window = GEDIT_WINDOW (g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
llmghost_plugin_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (object);
  switch (prop_id)
    {
    case PROP_WINDOW:
      g_value_set_object (value, self->window);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
llmghost_plugin_finalize (GObject *object)
{
  LlmghostPlugin *self = LLMGHOST_PLUGIN (object);
  g_clear_object (&self->backend);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (llmghost_plugin_parent_class)->finalize (object);
}

static void
llmghost_plugin_init (LlmghostPlugin *self)
{
  (void) self;
}

static void
llmghost_plugin_class_init (LlmghostPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->set_property = llmghost_plugin_set_property;
  object_class->get_property = llmghost_plugin_get_property;
  object_class->finalize     = llmghost_plugin_finalize;

  /* The "window" property is declared by GeditWindowActivatable; we just
   * implement it. */
  g_object_class_override_property (object_class, PROP_WINDOW, "window");
}

static void
llmghost_plugin_class_finalize (LlmghostPluginClass *klass)
{
  (void) klass;
}

/* ---- libpeas registration ---------------------------------------------- */

G_MODULE_EXPORT void
peas_register_types (PeasObjectModule *module)
{
  llmghost_plugin_register_type (G_TYPE_MODULE (module));
  peas_object_module_register_extension_type (module,
                                              GEDIT_TYPE_WINDOW_ACTIVATABLE,
                                              LLMGHOST_TYPE_PLUGIN);
  llmghost_configurable_register (module);
}
