#pragma once

#include <glib-object.h>
#include <gmodule.h>
#include <libpeas/peas.h>

G_BEGIN_DECLS

#define LLMGHOST_TYPE_CONFIGURABLE (llmghost_configurable_get_type())
G_DECLARE_FINAL_TYPE (LlmghostConfigurable, llmghost_configurable,
                      LLMGHOST, CONFIGURABLE, PeasExtensionBase)

/* Register the type and the PeasGtkConfigurable extension on @module. */
void llmghost_configurable_register (PeasObjectModule *module);

G_END_DECLS
