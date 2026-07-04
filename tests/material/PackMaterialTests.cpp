#include <doctest/doctest.h>
#include "material/MaterialAsset.hpp"
#include "material/MaterialPacking.hpp"
#include "material/TextureRegistry.hpp"
#include "FakeTextureSink.hpp"

using namespace aether;

TEST_CASE("PackMaterial copies PBR factors verbatim") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);

    MaterialAsset a;
    a.baseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
    a.metallicFactor = 0.6f;
    a.roughnessFactor = 0.7f;
    a.occlusionStrength = 0.8f;
    a.alphaCutoff = 0.25f;
    a.emissiveFactor = {1.0f, 0.5f, 0.0f};

    GpuMaterial g = PackMaterial(a, tex);

    CHECK(g.baseColorFactor.x == doctest::Approx(0.1f));
    CHECK(g.baseColorFactor.w == doctest::Approx(0.4f));
    CHECK(g.metallicFactor == doctest::Approx(0.6f));
    CHECK(g.roughnessFactor == doctest::Approx(0.7f));
    CHECK(g.occlusionStrength == doctest::Approx(0.8f));
    CHECK(g.alphaCutoff == doctest::Approx(0.25f));
    CHECK(g.emissiveFactor.x == doctest::Approx(1.0f));
    CHECK(g.emissiveFactor.z == doctest::Approx(0.0f));
}

TEST_CASE("PackMaterial packs bools into flags") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);

    MaterialAsset a;
    a.doubleSided = true;
    a.alphaMask = true;
    a.modulateVertexColor = true;
    a.alphaBlend = false;

    GpuMaterial g = PackMaterial(a, tex);

    CHECK((g.flags & GpuMaterial::kDoubleSided) != 0u);
    CHECK((g.flags & GpuMaterial::kAlphaMask) != 0u);
    CHECK((g.flags & GpuMaterial::kModulateVertexColor) != 0u);
    CHECK((g.flags & GpuMaterial::kAlphaBlend) == 0u);
}

TEST_CASE("PackMaterial resolves handles to heap slots and freezes the 80-byte layout") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    static_assert(sizeof(GpuMaterial) == 80);

    MaterialAsset a;
    a.albedoTex = tex.Acquire("brick.png");
    const std::uint32_t expected = tex.ResolveSlot(a.albedoTex);

    GpuMaterial g = PackMaterial(a, tex);
    CHECK(g.albedoSlot == expected);
    CHECK(g.normalSlot == GpuMaterial::kNoTexture); // default-constructed handle -> skip sample
}

TEST_CASE("PackMaterial distinguishes a broken (requested-but-missing) map from an absent one") {
    FakeTextureSink texSink(8);
    TextureRegistry tex(texSink);
    tex.InitializeDefault(TextureResource{99u}); // magenta fallback lives at slot 99

    MaterialAsset a;
    a.albedoTex = TextureHandle::Broken(); // texture requested, load failed
    // normalTex left default-constructed = "no normal map requested"

    GpuMaterial g = PackMaterial(a, tex);
    CHECK(g.albedoSlot == 99u);                     // broken -> VISIBLE magenta default
    CHECK(g.normalSlot == GpuMaterial::kNoTexture); // absent -> skip the sample (base response)
}
