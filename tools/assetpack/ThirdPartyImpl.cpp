// ---------------------------------------------------------------------------
// Single-header library implementations compiled once into the AssetPipeline
// library. stb_image is NOT here: it lives in StbImageImpl.cpp (exe-only) so the
// library can be linked into the engine, which already defines stb_image.
// ---------------------------------------------------------------------------

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include <xxhash.h>
