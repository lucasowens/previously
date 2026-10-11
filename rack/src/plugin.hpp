// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack plugin: shared declarations.
//
// Rack's SDK uses its own naming (PascalCase types, camelCase methods). Where we override Rack
// methods we must use Rack's names; everything we define ourselves follows the project style.

#pragma once

#include <rack.hpp>

extern rack::Plugin* pluginInstance;  // NOLINT: name and linkage are required by the Rack SDK

extern rack::Model* model_previously;  // NOLINT(*-avoid-non-const-global-variables): Rack registry
