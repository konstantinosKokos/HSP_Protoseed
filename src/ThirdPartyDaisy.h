#pragma once

// ThirdPartyDaisy.h — patched for HSP PsychoEnhancer compatibility
// Removes the global daisysp.h include to avoid naming collisions
// while keeping DaisyDuino available via HaroldPCB.

#include "vendor/daisy/src/DaisyDSP.h"   // Core DSP types from DaisyDuino
#include "vendor/daisy/src/DaisyDuino.h" // Main DaisyDuino API

// NOTE:
// If a sketch needs specific DaisySP modules, include them locally in that sketch.

// This avoids polluting the global namespace with conflicting class names like Biquad.