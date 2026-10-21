#include <doctest/doctest.h>

#include "vulkan/GpuEnumConversions.hpp"

TEST_CASE("Color attachment clears preserve all channels across the Vulkan boundary")
{
	const aether::gpu::RenderingAttachmentInfo attachment{
	        .imageLayout = aether::gpu::ImageLayout::ColorAttachment,
	        .loadOp = aether::gpu::LoadOp::Clear,
	        .clearValue = aether::gpu::ClearColor(0.75f, 0.25f, 0.5f, 1.0f),
	};
	const auto converted = aether::gpu::ToVk(attachment);
	CHECK(converted.clearValue.color.float32[0] == doctest::Approx(0.75f));
	CHECK(converted.clearValue.color.float32[1] == doctest::Approx(0.25f));
	CHECK(converted.clearValue.color.float32[2] == doctest::Approx(0.5f));
	CHECK(converted.clearValue.color.float32[3] == doctest::Approx(1.0f));
}

TEST_CASE("Depth attachment clears preserve depth and stencil across the Vulkan boundary")
{
	const aether::gpu::RenderingAttachmentInfo attachment{
	        .imageLayout = aether::gpu::ImageLayout::DepthAttachment,
	        .loadOp = aether::gpu::LoadOp::Clear,
	        .clearValue = aether::gpu::ClearDepth(0.625f, 7),
	};
	const auto converted = aether::gpu::ToVk(attachment);
	CHECK(converted.clearValue.depthStencil.depth == doctest::Approx(0.625f));
	CHECK(converted.clearValue.depthStencil.stencil == 7);
}
