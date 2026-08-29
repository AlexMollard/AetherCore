// FrameConstants::prevViewProj - the input camera motion blur reprojects through.
//
// The shader half of that effect cannot be exercised here, but this half can, and it is
// the half most likely to be silently wrong: publishing this frame's matrix instead of
// last frame's yields zero velocity and an effect that simply never appears, while
// publishing an uninitialised identity yields a full-screen smear on a camera that never
// moved. Neither shows up as a crash or a validation error.

#include <doctest/doctest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "gpu/GpuDevice.hpp"
#include "rendering/RenderFramePacket.hpp"

using namespace aether;

namespace
{
	RenderFramePacket PacketLookingFrom(const glm::vec3& eye)
	{
		RenderFramePacket packet{};
		packet.hasCameraData = true;
		packet.view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		packet.proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
		packet.cameraWorldPos = glm::vec4(eye, 1.0f);
		return packet;
	}
} // namespace

TEST_CASE("prevViewProj carries the previous frame's camera, not this one's")
{
	// No Init: composing the constants is pure arithmetic over the packet and one
	// remembered matrix, so this needs no device.
	GpuDevice device;

	const RenderFramePacket first = PacketLookingFrom(glm::vec3(0.0f, 2.0f, 10.0f));
	const RenderFramePacket second = PacketLookingFrom(glm::vec3(6.0f, 2.0f, 10.0f));

	SUBCASE("the very first frame has no history, so velocity is zero rather than enormous")
	{
		const FrameConstants fc = device.ComposeBaseFrameConstants(first, glm::mat4(1.0f));
		CHECK(fc.prevViewProj == fc.viewProj);
	}

	SUBCASE("the second frame reports the first frame's matrix")
	{
		const FrameConstants a = device.ComposeBaseFrameConstants(first, glm::mat4(1.0f));
		const FrameConstants b = device.ComposeBaseFrameConstants(second, glm::mat4(1.0f));

		CHECK(b.prevViewProj == a.viewProj);
		// And it is genuinely the older one: a camera that moved must not report the two as
		// equal, or there is no velocity to blur along.
		CHECK(b.prevViewProj != b.viewProj);
	}

	SUBCASE("a camera that did not move produces no velocity")
	{
		(void) device.ComposeBaseFrameConstants(first, glm::mat4(1.0f));
		const FrameConstants held = device.ComposeBaseFrameConstants(first, glm::mat4(1.0f));
		CHECK(held.prevViewProj == held.viewProj);
	}
}
