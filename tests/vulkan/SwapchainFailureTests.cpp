#include <doctest/doctest.h>

#include "vulkan/Swapchain.hpp"

namespace
{
	VkResult s_acquireResult = VK_SUCCESS;
	int s_fenceResets = 0;

	VKAPI_ATTR VkResult VKAPI_CALL WaitReady(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)
	{
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL AcquireResult(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)
	{
		return s_acquireResult;
	}

	VKAPI_ATTR VkResult VKAPI_CALL RejectFenceReset(VkDevice, uint32_t, const VkFence*)
	{
		++s_fenceResets;
		// Prevent any actual command recording even if the acquisition gate regresses.
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	struct VulkanAcquisitionStubs
	{
		PFN_vkWaitForFences wait = vkWaitForFences;
		PFN_vkAcquireNextImageKHR acquire = vkAcquireNextImageKHR;
		PFN_vkResetFences reset = vkResetFences;

		VulkanAcquisitionStubs()
		{
			vkWaitForFences = &WaitReady;
			vkAcquireNextImageKHR = &AcquireResult;
			vkResetFences = &RejectFenceReset;
			s_fenceResets = 0;
		}

		~VulkanAcquisitionStubs()
		{
			vkWaitForFences = wait;
			vkAcquireNextImageKHR = acquire;
			vkResetFences = reset;
		}
	};
}

TEST_CASE("Swapchain failed acquisition never resets the submission fence")
{
	const VulkanAcquisitionStubs stubs;
	for (const auto result: {VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_TIMEOUT, VK_NOT_READY})
	{
		CAPTURE(result);
		s_acquireResult = result;
		s_fenceResets = 0;
		aether::Swapchain swapchain;
		CHECK_THROWS(swapchain.BeginFrame(VK_NULL_HANDLE));
		CHECK_FALSE(swapchain.IsFrameValid());
		CHECK(s_fenceResets == 0);
	}
}

TEST_CASE("Swapchain out-of-date acquisition requests recreation without resetting the fence")
{
	const VulkanAcquisitionStubs stubs;
	s_acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
	aether::Swapchain swapchain;
	CHECK_NOTHROW(swapchain.BeginFrame(VK_NULL_HANDLE));
	CHECK(swapchain.NeedsRecreation());
	CHECK_FALSE(swapchain.IsFrameValid());
	CHECK(s_fenceResets == 0);
}

TEST_CASE("Swapchain successful and suboptimal acquisition reach frame setup")
{
	const VulkanAcquisitionStubs stubs;
	for (const auto result: {VK_SUCCESS, VK_SUBOPTIMAL_KHR})
	{
		s_acquireResult = result;
		s_fenceResets = 0;
		aether::Swapchain swapchain;
		CHECK_THROWS(swapchain.BeginFrame(VK_NULL_HANDLE));
		CHECK(s_fenceResets == 1);
		CHECK(swapchain.NeedsRecreation() == (result == VK_SUBOPTIMAL_KHR));
	}
}
