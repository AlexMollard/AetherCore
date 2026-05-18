#pragma once

#include "daScript/daScript.h"

// Script-visible mirror structs for ECS components whose C++ layout isn't
// directly daScript-accessible.
//
// Pattern for adding a new component type in a future module:
//   1. Define struct ScriptX here with das::float3 / float / bool fields
//   2. Add MAKE_TYPE_FACTORY(ScriptX, ScriptX) directly below it
//   3. Register a ManagedStructureAnnotation in the relevant module .cpp
//   4. Write ONE get_X / set_X pair per component - scripts access fields directly
//
// NOTE: Structs > 16 bytes cannot be passed by value via das::cast<>.
//       Pass by pointer in the C++ binding (daScript handles the reference automatically).
//       Example: void das_set_X(World* w, uint32_t id, const ScriptX* x)
