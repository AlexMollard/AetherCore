#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialPacking.hpp"

using namespace aether;

TEST_CASE("PackMaterial copies PBR factors verbatim") {
    MaterialAsset a;
    a.baseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
    a.metallicFactor = 0.6f;
    a.roughnessFactor = 0.7f;
    a.occlusionStrength = 0.8f;
    a.alphaCutoff = 0.25f;
    a.emissiveFactor = {1.0f, 0.5f, 0.0f};

    GpuMaterial g = PackMaterial(a);

    CHECK(g.baseColorFactor.x == doctest::Approx(0.1f));
    CHECK(g.baseColorFactor.w == doctest::Approx(0.4f));
    CHECK(g.metallicFactor == doctest::Approx(0.6f));
    CHECK(g.roughnessFactor == doctest::Approx(0.7f));
    CHECK(g.occlusionStrength == doctest::Approx(0.8f));
    CHECK(g.alphaCutoff == doctest::Approx(0.25f));
    CHECK(g.emissiveFactor.x == doctest::Approx(1.0f));
    CHECK(g.emissiveFactor.z == doctest::Approx(0.0f));
}

TEST_CASE("PackMaterial packs bools into flags and copies texture slots") {
    MaterialAsset a;
    a.doubleSided = true;
    a.alphaMask = true;
    a.modulateVertexColor = true;
    a.alphaBlend = false;
    a.albedoSlot = 5;
    a.emissiveSlot = 9;

    GpuMaterial g = PackMaterial(a);

    CHECK((g.flags & GpuMaterial::kDoubleSided) != 0u);
    CHECK((g.flags & GpuMaterial::kAlphaMask) != 0u);
    CHECK((g.flags & GpuMaterial::kModulateVertexColor) != 0u);
    CHECK((g.flags & GpuMaterial::kAlphaBlend) == 0u);
    CHECK(g.albedoSlot == 5u);
    CHECK(g.emissiveSlot == 9u);
    CHECK(g.normalSlot == GpuMaterial::kNoTexture);
}
