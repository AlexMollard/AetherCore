// Scenery foliage soft edge (MaterialSystem softCutout): gltf_mesh drawn a second time, in the
// blended group without a depth write, keeping only the antialiased one-pixel ramp just outside
// the alpha cutoff. The plain gltf_mesh draw has already laid the cutout down with depth.
#define AETHER_FOLIAGE_EDGE_PASS 1
#include "gltf_mesh.slang"
