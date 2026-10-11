// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack plugin: entry point.

#include "plugin.hpp"

rack::Plugin* pluginInstance = nullptr;  // NOLINT: required by the Rack SDK

// Called by Rack when the plugin library loads.
void init(rack::Plugin* plugin)  // NOLINT(misc-use-internal-linkage): Rack's entry point
{
    pluginInstance = plugin;
    plugin->addModel(model_previously);
}
