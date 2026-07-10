// stb_image implementation, compiled ONCE into the AssetPacker executable only.
// The AssetPipeline library deliberately does NOT define this: when the library
// is linked into the editor (App), the engine already provides stb_image's
// implementation (src/engine/material/Texture.cpp), and a second definition
// would be a duplicate-symbol link error.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
