#pragma once
#include "rotation_core.hpp"
// Resolve these exports with GetProcAddress in the native integration host.
// Initialize MUST be invoked on the game's inventory thread; the bridge only
// binds the caller's thread ID and cannot discover the correct game thread.
using DZ62_InitializeFn = std::uint32_t (*)();
using DZ62_SetExistingFootprintFn = std::uint32_t (*)(dz62::Cargo*, const dz62::Request*);
using DZ62_MarkScriptDirtyFn = std::uint32_t (*)(dz62::Cargo*, void*);
