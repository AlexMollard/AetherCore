#include <doctest/doctest.h>

#include <array>

#include "gpu/TextureUpload.hpp"

using namespace aether;

TEST_CASE("Texture upload validates complete RGBA and float mip chains")
{
	const gpu::ImageUploadRegion rgba[] = {{.offset = 0, .width = 3, .height = 5}, {.offset = 60, .width = 1, .height = 2, .mipLevel = 1}, {.offset = 68, .width = 1, .height = 1, .mipLevel = 2}};
	CHECK(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Srgb, {3, 5}, 3, 72, rgba).has_value());
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Srgb, {3, 5}, 3, 71, rgba).has_value());
	const gpu::ImageUploadRegion font{.width = 2, .height = 3};
	CHECK(gpu::ValidateTextureUpload(gpu::Format::R32G32B32A32Sfloat, {2, 3}, 1, 96, std::span(&font, 1)).has_value());
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R32G32B32A32Sfloat, {2, 3}, 1, 95, std::span(&font, 1)).has_value());
}

TEST_CASE("Texture upload validates partial compressed edge blocks and source alignment")
{
	const gpu::ImageUploadRegion bc7[] = {{.width = 7, .height = 5}, {.offset = 64, .width = 3, .height = 2, .mipLevel = 1}, {.offset = 80, .width = 1, .height = 1, .mipLevel = 2}};
	CHECK(gpu::ValidateTextureUpload(gpu::Format::BC7UnormBlock, {7, 5}, 3, 96, bc7).has_value());
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::BC7UnormBlock, {7, 5}, 3, 95, bc7).has_value());
	auto bad = std::to_array(bc7);
	bad[1].offset = 65;
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::BC7UnormBlock, {7, 5}, 3, 97, bad).has_value());
	const gpu::ImageUploadRegion bc4{.width = 1, .height = 1};
	CHECK(gpu::ValidateTextureUpload(gpu::Format::BC4UnormBlock, {1, 1}, 1, 8, std::span(&bc4, 1)).has_value());
}

TEST_CASE("Texture upload rejects missing duplicate and out-of-bounds regions")
{
	std::array<gpu::ImageUploadRegion, 2> regions{{{.width = 4, .height = 4}, {.offset = 64, .width = 2, .height = 2, .mipLevel = 1}}};
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Unorm, {4, 4}, 2, 80, std::span(regions).first(1)).has_value());
	regions[1].mipLevel = 0;
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Unorm, {4, 4}, 2, 80, regions).has_value());
	regions[1].mipLevel = 1;
	regions[1].x = 1;
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Unorm, {4, 4}, 2, 80, regions).has_value());
	regions[1].x = 0;
	regions[1].offset = UINT64_MAX;
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Unorm, {4, 4}, 2, 80, regions).has_value());
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::Undefined, {4, 4}, 2, 80, regions).has_value());
	CHECK_FALSE(gpu::ValidateTextureUpload(gpu::Format::R8G8B8A8Unorm, {0, 4}, 2, 80, regions).has_value());
}
