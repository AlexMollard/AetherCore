#include "vulkan/VulkanContext.hpp"

#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "utils/AetherExceptions.hpp"
#include "rendering/CommandRecorder.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "platform/Window.hpp"

// #define VULKAN_GPU_DEBUG
// #define VULKAN_CPU_DEBUG


#if defined(VULKAN_GPU_DEBUG) && defined(VULKAN_CPU_DEBUG)
#	error "VULKAN_GPU_DEBUG and VULKAN_CPU_DEBUG are mutually exclusive"
#endif

namespace
{
	VKAPI_ATTR VkBool32 VKAPI_CALL LogValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
	{
		(void) userData;

		const char* type = vkb::to_string_message_type(messageType);
		const char* message = callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage : "Unknown validation layer message.";

		if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
			aether::Logger::ErrorAt(aether::LogCategory::Validation, std::source_location::current(), "{}: {}", type, message);
		}
		else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
		{
			aether::Logger::WarnAt(aether::LogCategory::Validation, std::source_location::current(), "{}: {}", type, message);
		}
		else
		{
			aether::Logger::VerboseAt(aether::LogCategory::Validation, std::source_location::current(), "{}: {}", type, message);
		}

		return VK_FALSE;
	}
} // namespace

namespace aether
{
	VulkanContext::VulkanContext(const Window& window, const char* appName)
	{
		AE_INFO(LogCategory::Vulkan, "Creating Vulkan context for '{}'.", appName);

		// Initialize volk loader (loads global Vulkan functions)
		if (volkInitialize() != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to initialize volk Vulkan loader."));
		}

		VkDebugUtilsMessageSeverityFlagsEXT debugSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

		VkDebugUtilsMessageTypeFlagsEXT debugTypes = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

#if defined(VULKAN_GPU_DEBUG) || defined(VULKAN_CPU_DEBUG)
		debugSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
#endif

		vkb::InstanceBuilder instanceBuilder;
		instanceBuilder.set_app_name(appName);
		instanceBuilder.require_api_version(1, 4, 0);
		instanceBuilder.request_validation_layers();
		instanceBuilder.set_debug_callback(LogValidationMessage);
		instanceBuilder.set_debug_messenger_severity(debugSeverity);
		instanceBuilder.set_debug_messenger_type(debugTypes);

#if defined(VULKAN_GPU_DEBUG)
		instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
		instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
		instanceBuilder.add_validation_feature_disable(VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT);
		AE_INFO(LogCategory::Vulkan, "GPU-AV + Synchronization Validation enabled.");
#elif defined(VULKAN_CPU_DEBUG)
		AE_INFO(LogCategory::Vulkan, "Core Validation (CPU) enabled.");
#endif

		auto instanceResult = instanceBuilder.build();

		if (!instanceResult)
		{
			Throw(AetherError::Vulkan(0, std::string("Failed to create Vulkan instance: ") + instanceResult.error().message()));
		}

		m_instance = instanceResult.value();

		// Load instance-level Vulkan functions
		volkLoadInstance(m_instance->instance);

		if (glfwCreateWindowSurface(m_instance->instance, window.GetHandle(), nullptr, &m_surface) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to create Vulkan surface."));
		}

		VkPhysicalDeviceVulkan11Features requiredFeatures11{};
		requiredFeatures11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		requiredFeatures11.shaderDrawParameters = VK_TRUE;

		VkPhysicalDeviceVulkan12Features requiredFeatures12{};
		requiredFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		requiredFeatures12.bufferDeviceAddress = VK_TRUE;
		requiredFeatures12.descriptorIndexing = VK_TRUE;
		requiredFeatures12.scalarBlockLayout = VK_TRUE; // allows tight-packed structs in BDA/SSBO
		requiredFeatures12.runtimeDescriptorArray = VK_TRUE;
		requiredFeatures12.descriptorBindingPartiallyBound = VK_TRUE;
		requiredFeatures12.descriptorBindingVariableDescriptorCount = VK_TRUE;
		requiredFeatures12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		requiredFeatures12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		requiredFeatures12.timelineSemaphore = VK_TRUE;
		requiredFeatures12.drawIndirectCount = VK_TRUE;
		requiredFeatures12.shaderInt8 = VK_TRUE;
		requiredFeatures12.hostQueryReset = VK_TRUE;

		VkPhysicalDeviceVulkan13Features requiredFeatures13{};
		requiredFeatures13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		requiredFeatures13.dynamicRendering = VK_TRUE;
		requiredFeatures13.synchronization2 = VK_TRUE;

		VkPhysicalDeviceFeatures requiredFeatures10{};
		requiredFeatures10.shaderInt64 = VK_TRUE;
		requiredFeatures10.shaderInt16 = VK_TRUE;
		requiredFeatures10.multiDrawIndirect = VK_TRUE;
		requiredFeatures10.drawIndirectFirstInstance = VK_TRUE;

		vkb::PhysicalDeviceSelector selector{ *m_instance };
		selector.set_surface(m_surface).set_minimum_version(1, 4).set_required_features(requiredFeatures10).set_required_features_11(requiredFeatures11).set_required_features_12(requiredFeatures12).set_required_features_13(requiredFeatures13);
#ifdef TRACY_ENABLE
		// VK_EXT_calibrated_timestamps is required for Tracy host-calibrated GPU zones.
		// It is promoted to core in Vulkan 1.4 under the KHR name, but we request the EXT
		// extension explicitly so vkb enables it and the function pointers are available.
		selector.add_required_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif
		// maintenance9 allows queue family ownership transfers to be omitted when both
		// queue families are compatible, eliminating unnecessary barriers.
		selector.add_required_extension(VK_KHR_MAINTENANCE_9_EXTENSION_NAME);

		auto physicalDeviceResult = selector.select();

		if (!physicalDeviceResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to select a suitable Vulkan physical device."));
		}

		VkPhysicalDeviceMaintenance9FeaturesKHR maintenance9Features{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR,
			.maintenance9 = VK_TRUE,
		};

		vkb::DeviceBuilder deviceBuilder{ physicalDeviceResult.value() };
		deviceBuilder.add_pNext(&maintenance9Features);
		auto deviceResult = deviceBuilder.build();
		if (!deviceResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to create Vulkan logical device."));
		}

		m_device = deviceResult.value();

		// Load device-level Vulkan functions
		volkLoadDevice(m_device->device);

		const auto graphicsQueueResult = m_device->get_queue(vkb::QueueType::graphics);
		if (!graphicsQueueResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to get graphics queue."));
		}
		m_graphicsQueue = graphicsQueueResult.value();
		m_graphicsQueueFamily = m_device->get_queue_index(vkb::QueueType::graphics).value();
		m_computeQueue = m_graphicsQueue;
		m_computeQueueFamily = m_graphicsQueueFamily;

		const auto computeQueueResult = m_device->get_queue(vkb::QueueType::compute);
		if (computeQueueResult)
		{
			m_computeQueue = computeQueueResult.value();
			if (const auto computeIndex = m_device->get_queue_index(vkb::QueueType::compute))
			{
				m_computeQueueFamily = computeIndex.value();
			}
		}

		const auto presentQueueResult = m_device->get_queue(vkb::QueueType::present);
		if (!presentQueueResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to get present queue."));
		}
		m_presentQueue = presentQueueResult.value();

		CommandRecorder::SetDebugLabelFunctions(reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(m_device->device, "vkCmdBeginDebugUtilsLabelEXT")), reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(m_device->device, "vkCmdEndDebugUtilsLabelEXT")));

		const auto setObjectNameFn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(m_device->device, "vkSetDebugUtilsObjectNameEXT"));
		CommandRecorder::SetObjectNameFunction(setObjectNameFn);
		UniqueBuffer::SetObjectNameFunction(setObjectNameFn);

		// Name queues immediately so they appear correctly in RenderDoc and validation output.
		// Graphics and compute may be the same queue on some hardware; guard against double-naming.
		CommandRecorder::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(m_graphicsQueue), VK_OBJECT_TYPE_QUEUE, "Queue.Graphics");
		if (m_computeQueue != m_graphicsQueue)
		{
			CommandRecorder::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(m_computeQueue), VK_OBJECT_TYPE_QUEUE, "Queue.Compute");
		}
		if (m_presentQueue != m_graphicsQueue && m_presentQueue != m_computeQueue)
		{
			CommandRecorder::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(m_presentQueue), VK_OBJECT_TYPE_QUEUE, "Queue.Present");
		}

		// Report GPU (VkDeviceMemory) allocations to Tracy as the "GPU" named pool
		// so VRAM usage is visible alongside CPU heap allocations.
#ifdef TRACY_ENABLE
		static const VmaDeviceMemoryCallbacks kTracyVmaCallbacks{
			.pfnAllocate = [](VmaAllocator, uint32_t, VkDeviceMemory memory, VkDeviceSize size, void*) { AE_PROFILE_ALLOC_N(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memory)), static_cast<std::size_t>(size), "GPU"); },
			.pfnFree = [](VmaAllocator, uint32_t, VkDeviceMemory memory, VkDeviceSize, void*) { AE_PROFILE_FREE_N(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memory)), "GPU"); },
		};
#endif

		VmaVulkanFunctions vulkanFunctions{};
		vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
		vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

		VmaAllocatorCreateInfo allocatorCreateInfo{};
		allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
		allocatorCreateInfo.physicalDevice = physicalDeviceResult.value().physical_device;
		allocatorCreateInfo.device = m_device->device;
		allocatorCreateInfo.instance = m_instance->instance;
		allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
		allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
#ifdef TRACY_ENABLE
		allocatorCreateInfo.pDeviceMemoryCallbacks = &kTracyVmaCallbacks;
#endif

		const VkResult allocatorResult = vmaCreateAllocator(&allocatorCreateInfo, &m_allocator);
		if (allocatorResult != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(static_cast<int32_t>(allocatorResult), std::format("Failed to create VMA allocator. VkResult={}", static_cast<int>(allocatorResult))));
		}

#ifdef TRACY_ENABLE
		{
			const auto qpreset = reinterpret_cast<PFN_vkResetQueryPoolEXT>(vkGetDeviceProcAddr(m_device->device, "vkResetQueryPool"));
			const auto gpdctd = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(vkGetInstanceProcAddr(m_instance->instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
			const auto gct = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetDeviceProcAddr(m_device->device, "vkGetCalibratedTimestampsEXT"));

			if (!qpreset || !gpdctd || !gct)
			{
				Throw(AetherError::Vulkan(0, "Tracy GPU context requires VK_EXT_calibrated_timestamps but function pointers are null. Ensure the extension is supported by the driver."));
			}

			m_tracyVkCtx = TracyVkContextHostCalibrated(physicalDeviceResult.value().physical_device, m_device->device, qpreset, gpdctd, gct);
			AE_PROFILE_GPU_CONTEXT_NAME(m_tracyVkCtx, "AetherCore GPU");
		}
#endif

		AE_INFO(LogCategory::Vulkan, "Vulkan context initialized successfully.");
	}

	VulkanContext::~VulkanContext()
	{
		AE_VERBOSE(LogCategory::Vulkan, "Destroying Vulkan context resources.");

		if (m_allocator != VK_NULL_HANDLE)
		{
			vmaDestroyAllocator(m_allocator);
			m_allocator = VK_NULL_HANDLE;
		}

#ifdef TRACY_ENABLE
		if (m_tracyVkCtx)
		{
			TracyVkDestroy(m_tracyVkCtx);
			m_tracyVkCtx = nullptr;
		}
#endif

		CommandRecorder::SetDebugLabelFunctions(nullptr, nullptr);
		CommandRecorder::SetObjectNameFunction(nullptr);
		UniqueBuffer::SetObjectNameFunction(nullptr);

		if (m_device.has_value())
		{
			vkb::destroy_device(*m_device);
		}

		if (m_surface != VK_NULL_HANDLE && m_instance.has_value())
		{
			vkb::destroy_surface(*m_instance, m_surface);
		}

		if (m_instance.has_value())
		{
			vkb::destroy_instance(*m_instance);
		}
	}

	const vkb::Instance& VulkanContext::GetInstance() const
	{
		return *m_instance;
	}

	const vkb::Device& VulkanContext::GetDevice() const
	{
		return *m_device;
	}

	VkPhysicalDevice VulkanContext::GetPhysicalDevice() const
	{
		return m_device->physical_device;
	}

	VkSurfaceKHR VulkanContext::GetSurface() const
	{
		return m_surface;
	}

	VmaAllocator VulkanContext::GetAllocator() const
	{
		return m_allocator;
	}

	VkQueue VulkanContext::GetGraphicsQueue() const
	{
		return m_graphicsQueue;
	}

	VkQueue VulkanContext::GetComputeQueue() const
	{
		return m_computeQueue;
	}

	VkQueue VulkanContext::GetPresentQueue() const
	{
		return m_presentQueue;
	}

	std::uint32_t VulkanContext::GetGraphicsQueueFamily() const
	{
		return m_graphicsQueueFamily;
	}

	std::uint32_t VulkanContext::GetComputeQueueFamily() const
	{
		return m_computeQueueFamily;
	}
} // namespace aether
