#pragma once

#include <glib-object.h>
#include <gmodule.h>
#include <libpeas/peas.h>

G_BEGIN_DECLS

#define LLMGHOST_TYPE_PLUGIN (llmghost_plugin_get_type())
G_DECLARE_FINAL_TYPE (LlmghostPlugin, llmghost_plugin,
                      LLMGHOST, PLUGIN, PeasExtensionBase)

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

G_END_DECLS
