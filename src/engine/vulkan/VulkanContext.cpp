#include "vulkan/VulkanContext.hpp"

#include "Defines.hpp"

#if defined(VULKAN_GPU_DEBUG)
#	define VK_VALIDATION_CPU 1
#	define VK_VALIDATION_GPU 1
#elif defined(VULKAN_CPU_DEBUG)
#	define VK_VALIDATION_CPU 1
#	define VK_VALIDATION_GPU 0
#else
#	define VK_VALIDATION_CPU 0
#	define VK_VALIDATION_GPU 0
#endif

#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "utils/AetherExceptions.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuProfiler.hpp"
#include "vulkan/TracyGpuProfiler.hpp"
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
#	include <tracy/TracyVulkan.hpp>
#endif
#include "platform/CrashHandler.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/GpuMemoryTracker.hpp"
#include "platform/Window.hpp"
#include "io/PlatformPaths.hpp"

namespace
{
	aether::GpuMemoryTracker* g_addressBindingTracker = nullptr;

	const char* ObjectTypeToString(VkObjectType type)
	{
		switch (static_cast<int>(type))
		{
			case VK_OBJECT_TYPE_IMAGE:
				return "Image";
			case VK_OBJECT_TYPE_IMAGE_VIEW:
				return "ImageView";
			case VK_OBJECT_TYPE_BUFFER:
				return "Buffer";
			case VK_OBJECT_TYPE_BUFFER_VIEW:
				return "BufferView";
			case VK_OBJECT_TYPE_SHADER_MODULE:
				return "ShaderModule";
			case VK_OBJECT_TYPE_PIPELINE:
				return "Pipeline";
			case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
				return "PipelineLayout";
			case VK_OBJECT_TYPE_DESCRIPTOR_SET:
				return "DescriptorSet";
			case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
				return "DescriptorSetLayout";
			case VK_OBJECT_TYPE_SAMPLER:
				return "Sampler";
			case VK_OBJECT_TYPE_COMMAND_BUFFER:
				return "CommandBuffer";
			case VK_OBJECT_TYPE_RENDER_PASS:
				return "RenderPass";
			case VK_OBJECT_TYPE_FRAMEBUFFER:
				return "Framebuffer";
			case VK_OBJECT_TYPE_DEVICE:
				return "Device";
			case VK_OBJECT_TYPE_QUEUE:
				return "Queue";
			case VK_OBJECT_TYPE_SEMAPHORE:
				return "Semaphore";
			case VK_OBJECT_TYPE_FENCE:
				return "Fence";
			case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
				return "Swapchain";
			case VK_OBJECT_TYPE_SURFACE_KHR:
				return "Surface";
			case VK_OBJECT_TYPE_INSTANCE:
				return "Instance";
			default:
				return "Unknown";
		}
	}

	bool IsSuppressedMessage(const char* message)
	{
		if (message == nullptr)
		{
			return false;
		}
		const std::string_view msg(message);
		if (msg.contains("DebugPrintf logs to the Information"))
		{
			return true;
		}
		if (msg.contains("Khronos Validation Layer Active"))
		{
			return true;
		}
		if (msg.contains("Cannot open shader validation cache"))
		{
			return true;
		}
		// "vkCreateDevice(): Warning that validation is adjusting settings:
		if (msg.contains("validation is adjusting settings"))
		{
			return true;
		}
		// Non-actionable "Internal Warning" diagnostics from the validation
		if (msg.contains("Internal Warning"))
		{
			return true;
		}
		if (msg.contains("should be sub-allocated"))
		{
			return true;
		}
		return false;
	}

	VKAPI_ATTR VkBool32 VKAPI_CALL LogValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
	{
		(void) userData;

		// Early-out for known-noisy messages so they never reach the logger.
		if (callbackData != nullptr && IsSuppressedMessage(callbackData->pMessage))
		{
			return VK_FALSE;
		}

		const char* type = vkb::to_string_message_type(messageType);
		const char* message = callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage : "Unknown validation layer message.";

		std::string objects;
		if (callbackData != nullptr && callbackData->objectCount > 0 && callbackData->pObjects != nullptr)
		{
			objects = " (objects: ";
			for (uint32_t i = 0; i < callbackData->objectCount; ++i)
			{
				if (i > 0)
				{
					objects += ", ";
				}
				const auto& obj = callbackData->pObjects[i];
				if (obj.pObjectName != nullptr && obj.pObjectName[0] != '\0')
				{
					objects += obj.pObjectName;
				}
				else
				{
					objects += ObjectTypeToString(obj.objectType);
				}
			}
			objects += ")";
		}

		const std::string extra;
		if (callbackData != nullptr)
		{
			for (const VkBaseInStructure* pNext = static_cast<const VkBaseInStructure*>(callbackData->pNext); pNext != nullptr; pNext = pNext->pNext)
			{
				if (pNext->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT)
				{
					const auto* binding = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(pNext);
					if (g_addressBindingTracker != nullptr)
					{
						if (binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT)
						{
							g_addressBindingTracker->Register(binding->baseAddress, binding->size, "<address-binding>", aether::GpuMemoryTracker::ResourceType::Buffer);
						}
						else if (binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT)
						{
							g_addressBindingTracker->UnregisterRange(binding->baseAddress, binding->size);
						}
					}
				}
			}
		}

		const std::string decorated = std::string(type) + ": " + message + objects + extra;

		if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
			aether::Logger::ErrorAt(aether::LogCategory::Validation, std::source_location::current(), "{}", decorated);
		}
		else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
		{
			aether::Logger::WarnAt(aether::LogCategory::Validation, std::source_location::current(), "{}", decorated);
		}
		else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0 && (messageType & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0)
		{
			// best-practices advisories) and must surface in the log. INFO-severity
			aether::Logger::InfoAt(aether::LogCategory::Validation, std::source_location::current(), "{}", decorated);
		}
		else
		{
			aether::Logger::VerboseAt(aether::LogCategory::Validation, std::source_location::current(), "{}", decorated);
		}

		return VK_FALSE;
	}

	// game never writes into its own install directory. Lives under
	std::filesystem::path ResolveGpuCacheDir(std::string_view subdir)
	{
		std::filesystem::path base = aether::io::PlatformPaths::GetUserConfigDir();
		if (base.empty())
		{
			std::error_code cwdError;
			base = std::filesystem::current_path(cwdError);
		}

		std::string exeName = aether::io::PlatformPaths::GetExecutableName();
		if (exeName.empty())
		{
			exeName = "AetherCore";
		}

		std::filesystem::path dir = base / "cache" / exeName / subdir;
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec)
		{
			AE_WARN(aether::LogCategory::Vulkan, "Failed to create GPU cache directory '{}': {}", dir.string(), ec.message());
		}
		return dir;
	}
} // namespace

namespace aether
{
	VulkanContext::VulkanContext(const Window& window, const char* appName, [[maybe_unused]] bool enableGpuDiagnostics, [[maybe_unused]] bool enableValidation)
	{
		AE_PROFILE_ZONE();
		AE_INFO(LogCategory::Vulkan, "Creating Vulkan context for '{}'.", appName);

		if (volkInitialize() != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to initialize volk Vulkan loader."));
		}

		vkb::InstanceBuilder instanceBuilder;
		instanceBuilder.set_app_name(appName);
		instanceBuilder.require_api_version(1, 4, 0);
#if VK_VALIDATION_CPU
		// The validation layer is compiled in for dev builds, but it accumulates state
		// (~2-3 KB/frame) and progressively slows the render thread over long sessions,
		// so allow disabling it at startup (--no-validation) for a stable high frame rate.
		if (enableValidation)
		{
			VkDebugUtilsMessageSeverityFlagsEXT debugSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			const VkDebugUtilsMessageTypeFlagsEXT debugTypes = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			debugSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
			instanceBuilder.request_validation_layers();
			instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

			instanceBuilder.set_debug_callback(LogValidationMessage);
			instanceBuilder.set_debug_messenger_severity(debugSeverity);
			instanceBuilder.set_debug_messenger_type(debugTypes);

#	if defined(VULKAN_BEST_PRACTICES)
			instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
#	endif
#	if VK_VALIDATION_GPU
			instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
			instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
			instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
			instanceBuilder.add_validation_feature_disable(VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT);
			AE_INFO(LogCategory::Vulkan, "Vulkan validation layer enabled (GPU-AV, debug printf + best practices; render-pass injection, TDR risk on AMD/Intel).");
#	else
			instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
			AE_INFO(LogCategory::Vulkan, "Vulkan validation layer enabled (sync validation + best practices).");
#	endif
		}
		else
		{
			AE_INFO(LogCategory::Vulkan, "Vulkan validation layer disabled at startup (--no-validation) - stable frame rate, no validation diagnostics.");
		}
#endif

		auto instanceResult = instanceBuilder.build();

		if (!instanceResult)
		{
			Throw(AetherError::Vulkan(0, std::string("Failed to create Vulkan instance: ") + instanceResult.error().message()));
		}

		m_instance = instanceResult.value();

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
		requiredFeatures12.scalarBlockLayout = VK_TRUE;
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
		requiredFeatures10.fillModeNonSolid = VK_TRUE;
		requiredFeatures10.wideLines = VK_TRUE;

		vkb::PhysicalDeviceSelector selector{*m_instance};
		selector.set_surface(m_surface).set_minimum_version(1, 4).set_required_features(requiredFeatures10).set_required_features_11(requiredFeatures11).set_required_features_12(requiredFeatures12).set_required_features_13(requiredFeatures13);
		{
			const VkPhysicalDeviceVulkan14Features features14{
			        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
			        .hostImageCopy = VK_TRUE,
			        .pushDescriptor = VK_TRUE,
			};
			selector.set_required_features_14(features14);
		}
#ifdef TRACY_ENABLE
		// VK_EXT_calibrated_timestamps is required for Tracy host-calibrated GPU zones.
		selector.add_required_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif
		// VK_KHR_maintenance9: optional device extension. Required by this renderer
		selector.add_required_extension(VK_KHR_MAINTENANCE_9_EXTENSION_NAME);
		selector.add_required_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
		// VK_EXT_shader_object: layout-free shaders bound directly via vkCmdBindShadersEXT.
		selector.add_required_extension(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
		// an extension and is required explicitly below because shader objects use EDS3
		selector.add_required_extension(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		selector.add_required_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
		selector.add_required_extension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
		selector.add_required_extension(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
		// picks one. Requesting them as *required* selector extensions this

		auto physicalDeviceResult = selector.select();

		if (!physicalDeviceResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to select a suitable Vulkan physical device."));
		}

		// gated on two independent conditions, both required:
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		const bool aftermathDeviceExtensionsEnabled = false;
		{
			const std::uint32_t vendorId = physicalDeviceResult.value().properties.vendorID;
			constexpr std::uint32_t kVendorIdNvidia = 0x10DE;
			if (!enableGpuDiagnostics)
			{
				AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: not requested by this process (dev-only diagnostics; disabled for a shipped game runtime).");
			}
			else if (vendorId != kVendorIdNvidia)
			{
				AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: disabled - physical device vendorID=0x{:04X} is not NVIDIA (0x10DE); no NVIDIA-only extension requested.", vendorId);
			}
			else
			{
#	if VK_VALIDATION_CPU
				AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: disabled because the Vulkan validation layer is active (mutually incompatible; enable one or the other).");
#	else
				const bool diagnosticsConfigPresent = physicalDeviceResult.value().enable_extension_if_present(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
				const bool checkpointsPresent = physicalDeviceResult.value().enable_extension_if_present(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
				if (diagnosticsConfigPresent && checkpointsPresent)
				{
					// writes alongside them must not land in the game's install directory
					const std::string crashDumpDir = ResolveGpuCacheDir("gpu-crash-dumps").string();
					aftermathDeviceExtensionsEnabled = m_aftermathContext.EnableGpuCrashDumps(crashDumpDir.c_str());
					if (aftermathDeviceExtensionsEnabled)
					{
						AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: enabling GPU crash dumps (editor build, NVIDIA device '{}').", physicalDeviceResult.value().properties.deviceName);
					}
				}
				else
				{
					AE_WARN(LogCategory::Vulkan,
					        "NVIDIA Aftermath: NVIDIA device does not expose VK_NV_device_diagnostics_config/VK_NV_device_diagnostic_checkpoints (diagnosticsConfig={}, checkpoints={}); disabling.",
					        diagnosticsConfigPresent,
					        checkpointsPresent);
				}
#	endif
			}
		}
#endif

		// chained here. vkb owns the lifetime of the core feature structs it
		VkPhysicalDeviceMaintenance9FeaturesKHR maintenance9Features{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR,
#if defined(VULKAN_BEST_PRACTICES)
		        .maintenance9 = VK_FALSE,
#else
		        .maintenance9 = VK_TRUE,
#endif
		};

		VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
		        .shaderObject = VK_TRUE,
		};

		VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
		        .descriptorHeap = VK_TRUE,
		};

		VkPhysicalDeviceFaultFeaturesEXT supportedFaultFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
		};
		VkPhysicalDeviceFeatures2 queryFaultFeatures2{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		        .pNext = &supportedFaultFeatures,
		};
		vkGetPhysicalDeviceFeatures2(physicalDeviceResult.value().physical_device, &queryFaultFeatures2);

		VkPhysicalDeviceExtendedDynamicState3FeaturesEXT supportedEDS3{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
		};
		VkPhysicalDeviceFeatures2 queryEDS3Features2{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		        .pNext = &supportedEDS3,
		};
		vkGetPhysicalDeviceFeatures2(physicalDeviceResult.value().physical_device, &queryEDS3Features2);

		const bool eds3RequiredSupport = supportedEDS3.extendedDynamicState3PolygonMode == VK_TRUE && supportedEDS3.extendedDynamicState3RasterizationSamples == VK_TRUE && supportedEDS3.extendedDynamicState3SampleMask == VK_TRUE
		                                 && supportedEDS3.extendedDynamicState3AlphaToCoverageEnable == VK_TRUE && supportedEDS3.extendedDynamicState3LogicOpEnable == VK_TRUE && supportedEDS3.extendedDynamicState3ColorBlendEnable == VK_TRUE
		                                 && supportedEDS3.extendedDynamicState3ColorBlendEquation == VK_TRUE && supportedEDS3.extendedDynamicState3ColorWriteMask == VK_TRUE;
		if (!eds3RequiredSupport)
		{
			Throw(AetherError::Vulkan(0, "VK_EXT_extended_dynamic_state3 is missing required dynamic state features."));
		}
		if (supportedEDS3.extendedDynamicState3AlphaToOneEnable != VK_TRUE)
		{
			AE_INFO(LogCategory::Vulkan, "VK_EXT_extended_dynamic_state3 alpha-to-one dynamic state is not supported; leaving alpha-to-one disabled.");
		}

		VkPhysicalDeviceFaultFeaturesEXT faultFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
		        .deviceFault = supportedFaultFeatures.deviceFault,
		        .deviceFaultVendorBinary = supportedFaultFeatures.deviceFaultVendorBinary,
		};

		VkPhysicalDeviceAddressBindingReportFeaturesEXT addressBindingReportFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
		        .reportAddressBinding = VK_TRUE,
		};

		VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicStateFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
		        .extendedDynamicState = VK_TRUE,
		};

		VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extendedDynamicState2Features{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,
		        .extendedDynamicState2 = VK_TRUE,
		};

		VkPhysicalDeviceExtendedDynamicState3FeaturesEXT extendedDynamicState3Features{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
		        .extendedDynamicState3PolygonMode = supportedEDS3.extendedDynamicState3PolygonMode,
		        .extendedDynamicState3RasterizationSamples = supportedEDS3.extendedDynamicState3RasterizationSamples,
		        .extendedDynamicState3SampleMask = supportedEDS3.extendedDynamicState3SampleMask,
		        .extendedDynamicState3AlphaToCoverageEnable = supportedEDS3.extendedDynamicState3AlphaToCoverageEnable,
		        .extendedDynamicState3AlphaToOneEnable = supportedEDS3.extendedDynamicState3AlphaToOneEnable,
		        .extendedDynamicState3LogicOpEnable = supportedEDS3.extendedDynamicState3LogicOpEnable,
		        .extendedDynamicState3ColorBlendEnable = supportedEDS3.extendedDynamicState3ColorBlendEnable,
		        .extendedDynamicState3ColorBlendEquation = supportedEDS3.extendedDynamicState3ColorBlendEquation,
		        .extendedDynamicState3ColorWriteMask = supportedEDS3.extendedDynamicState3ColorWriteMask,
		};

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfig{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV,
		        .flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV,
		};
#endif

		vkb::DeviceBuilder deviceBuilder{physicalDeviceResult.value()};
		deviceBuilder.add_pNext(&maintenance9Features);
		deviceBuilder.add_pNext(&shaderObjectFeatures);
		deviceBuilder.add_pNext(&descriptorHeapFeatures);
		deviceBuilder.add_pNext(&faultFeatures);
		deviceBuilder.add_pNext(&addressBindingReportFeatures);
		deviceBuilder.add_pNext(&extendedDynamicStateFeatures);
		deviceBuilder.add_pNext(&extendedDynamicState2Features);
		deviceBuilder.add_pNext(&extendedDynamicState3Features);
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		if (aftermathDeviceExtensionsEnabled)
		{
			deviceBuilder.add_pNext(&diagnosticsConfig);
		}
#endif
		auto deviceResult = deviceBuilder.build();
		if (!deviceResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to create Vulkan logical device."));
		}

		m_device = deviceResult.value();

		volkLoadDevice(m_device->device);

		if (vkGetPhysicalDeviceToolPropertiesEXT != nullptr)
		{
			uint32_t toolCount = 0;
			if (vkGetPhysicalDeviceToolPropertiesEXT(m_device->physical_device, &toolCount, nullptr) == VK_SUCCESS && toolCount > 0)
			{
				std::vector<VkPhysicalDeviceToolPropertiesEXT> tools(toolCount);
				if (vkGetPhysicalDeviceToolPropertiesEXT(m_device->physical_device, &toolCount, tools.data()) == VK_SUCCESS)
				{
					AE_INFO(LogCategory::Vulkan, "Active Vulkan tools ({}):", toolCount);
					for (const auto& tool: tools)
					{
						AE_INFO(LogCategory::Vulkan, "  - {} v{}: {}", tool.name, tool.version, tool.description);
					}
				}
			}
		}

#if VK_VALIDATION_CPU
		m_debugMessenger = VK_NULL_HANDLE;
#endif

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

		gpu::CommandList::SetDebugLabelFunctions(reinterpret_cast<void*>(reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetDeviceProcAddr(m_device->device, "vkCmdBeginDebugUtilsLabelEXT"))),
		        reinterpret_cast<void*>(reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetDeviceProcAddr(m_device->device, "vkCmdEndDebugUtilsLabelEXT"))));
		gpu::CommandList::SetAlphaToOneDynamicStateSupported(extendedDynamicState3Features.extendedDynamicState3AlphaToOneEnable == VK_TRUE);

		const auto setObjectNameFn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(m_device->device, "vkSetDebugUtilsObjectNameEXT"));
		vkutil::SetObjectNameFunction(setObjectNameFn);

		vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_graphicsQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Graphics");
		if (m_computeQueue != m_graphicsQueue)
		{
			vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_computeQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Compute");
		}
		if (m_presentQueue != m_graphicsQueue && m_presentQueue != m_computeQueue)
		{
			vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_presentQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Present");
		}

		{
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(m_device->physical_device, &props);
			if (props.limits.maxPushConstantsSize < 128)
			{
				AE_WARN(LogCategory::Vulkan,
				        "maxPushConstantsSize is {} bytes (< 128). "
				        "Some compute push constants may fail to bind. "
				        "Update your GPU driver or hardware.",
				        props.limits.maxPushConstantsSize);
			}
			AE_INFO(LogCategory::Vulkan, "Physical device: {}, maxPushConstantsSize={}", props.deviceName, props.limits.maxPushConstantsSize);

			// is the first question when a fault lands in a driver DLL, and it must
			CrashHandler::SetContext(
			        "GPU", std::format("{} (Vulkan {}.{}.{}, driver 0x{:X})", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion));
		}

		{
			m_descriptorHeapProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
			VkPhysicalDeviceProperties2 props2{
			        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			        .pNext = &m_descriptorHeapProps,
			};
			vkGetPhysicalDeviceProperties2(m_device->physical_device, &props2);
			AE_INFO(LogCategory::Vulkan,
			        "Descriptor heap props: resourceHeapAlignment={}, imageDescriptorSize={}, samplerDescriptorSize={}, "
			        "maxResourceHeapSize={}, maxSamplerHeapSize={}, minResourceHeapReservedRange={}, minSamplerHeapReservedRange={}, maxPushDataSize={}",
			        m_descriptorHeapProps.resourceHeapAlignment,
			        m_descriptorHeapProps.imageDescriptorSize,
			        m_descriptorHeapProps.samplerDescriptorSize,
			        m_descriptorHeapProps.maxResourceHeapSize,
			        m_descriptorHeapProps.maxSamplerHeapSize,
			        m_descriptorHeapProps.minResourceHeapReservedRange,
			        m_descriptorHeapProps.minSamplerHeapReservedRange,
			        m_descriptorHeapProps.maxPushDataSize);
		}

		// LocalAppData (never CWD/the game's install directory -- a shipped game
		{
			m_pipelineCachePath = ResolveGpuCacheDir("pipeline") / "pipeline_cache.bin";

			std::vector<char> cacheData;
			if (std::ifstream inFile(m_pipelineCachePath, std::ios::binary | std::ios::ate); inFile.is_open())
			{
				const auto fileSize = inFile.tellg();
				if (fileSize > 0)
				{
					cacheData.resize(static_cast<std::size_t>(fileSize));
					inFile.seekg(0);
					inFile.read(cacheData.data(), fileSize);
				}
				inFile.close();
				AE_INFO(LogCategory::Vulkan, "Loaded pipeline cache from {} ({} bytes).", m_pipelineCachePath.string(), cacheData.size());
			}

			const VkPipelineCacheCreateInfo cacheInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
			        .initialDataSize = cacheData.size(),
			        .pInitialData = cacheData.empty() ? nullptr : cacheData.data(),
			};
			VkResult cacheResult = vkCreatePipelineCache(m_device->device, &cacheInfo, nullptr, &m_pipelineCache);
			if (cacheResult != VK_SUCCESS && cacheResult != VK_ERROR_OUT_OF_HOST_MEMORY)
			{
				AE_INFO(LogCategory::Vulkan, "Pipeline cache data rejected; creating fresh cache.");
				const VkPipelineCacheCreateInfo emptyInfo{
				        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
				};
				cacheResult = vkCreatePipelineCache(m_device->device, &emptyInfo, nullptr, &m_pipelineCache);
			}
			if (cacheResult != VK_SUCCESS)
			{
				AE_WARN(LogCategory::Vulkan, "Failed to create pipeline cache; falling back to no cache.");
				m_pipelineCache = VK_NULL_HANDLE;
			}
		}

		static const VmaDeviceMemoryCallbacks kVmaCallbacks{
		        .pfnAllocate =
		                [](VmaAllocator, uint32_t memoryType, VkDeviceMemory memory, VkDeviceSize size, void*)
		        {
			        (void) memoryType;
			        (void) size;
#ifdef TRACY_ENABLE
			        AE_PROFILE_ALLOC_N(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memory)), static_cast<std::size_t>(size), "GPU");
#endif
		        },
		        .pfnFree =
		                [](VmaAllocator, uint32_t memoryType, VkDeviceMemory memory, VkDeviceSize size, void*)
		        {
			        (void) memoryType;
			        (void) size;
#ifdef TRACY_ENABLE
			        AE_PROFILE_FREE_N(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(memory)), "GPU");
#endif
		        },
		};

		VmaVulkanFunctions vulkanFunctions{};
		vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
		vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

		VmaAllocatorCreateInfo allocatorCreateInfo{};
		allocatorCreateInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;
		allocatorCreateInfo.physicalDevice = physicalDeviceResult.value().physical_device;
		allocatorCreateInfo.device = m_device->device;
		allocatorCreateInfo.instance = m_instance->instance;
		allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_4;
		allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;
		allocatorCreateInfo.pDeviceMemoryCallbacks = &kVmaCallbacks;

		const VkResult allocatorResult = vmaCreateAllocator(&allocatorCreateInfo, &m_allocator);
		if (allocatorResult != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(static_cast<int32_t>(allocatorResult), std::format("Failed to create VMA allocator. VkResult={}", static_cast<int>(allocatorResult))));
		}

#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		{
			const auto qpreset = reinterpret_cast<PFN_vkResetQueryPoolEXT>(vkGetDeviceProcAddr(m_device->device, "vkResetQueryPool"));
			const auto gpdctd = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(vkGetInstanceProcAddr(m_instance->instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
			const auto gct = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetDeviceProcAddr(m_device->device, "vkGetCalibratedTimestampsEXT"));

			if (!qpreset || !gpdctd || !gct)
			{
				Throw(AetherError::Vulkan(0, "Tracy GPU context requires VK_EXT_calibrated_timestamps but function pointers are null. Ensure the extension is supported by the driver."));
			}

			m_tracyVkCtx = TracyVkContextHostCalibrated(physicalDeviceResult.value().physical_device, m_device->device, qpreset, gpdctd, gct);
			// and hand it to the engine singleton. Engine code never
			m_tracyProfilerHandle = aether::vulkan::CreateTracyGpuProfilerContext(m_tracyVkCtx);
			gpu::GpuProfiler::Get().Initialize({m_tracyProfilerHandle});
			gpu::GpuProfiler::Get().SetName("AetherCore GPU");
		}
#endif

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		if (aftermathDeviceExtensionsEnabled)
		{
			[[maybe_unused]] auto _ = m_aftermathContext.Initialize(m_device->device, physicalDeviceResult.value().physical_device);
		}
#endif

		AE_INFO(LogCategory::Vulkan, "Vulkan context initialized successfully.");
	}

	VulkanContext::~VulkanContext()
	{
		AE_PROFILE_ZONE();
		AE_VERBOSE(LogCategory::Vulkan, "Destroying Vulkan context resources.");

		if (m_pipelineCache != VK_NULL_HANDLE && m_device.has_value())
		{
			// Serialize the cache to disk so subsequent runs benefit from compiled pipelines.
			{
				std::size_t cacheSize = 0;
				vkGetPipelineCacheData(m_device->device, m_pipelineCache, &cacheSize, nullptr);
				if (cacheSize > 0)
				{
					std::vector<char> cacheData(cacheSize);
					const VkResult saveResult = vkGetPipelineCacheData(m_device->device, m_pipelineCache, &cacheSize, cacheData.data());
					if ((saveResult == VK_SUCCESS || saveResult == VK_INCOMPLETE) && !m_pipelineCachePath.empty())
					{
						if (std::ofstream outFile(m_pipelineCachePath, std::ios::binary); outFile.is_open())
						{
							outFile.write(cacheData.data(), static_cast<std::streamsize>(cacheSize));
							outFile.close();
							AE_INFO(LogCategory::Vulkan, "Saved pipeline cache to {} ({} bytes).", m_pipelineCachePath.string(), cacheSize);
						}
					}
				}
			}
			vkDestroyPipelineCache(m_device->device, m_pipelineCache, nullptr);
			m_pipelineCache = VK_NULL_HANDLE;
		}

		if (m_allocator != VK_NULL_HANDLE)
		{
			vmaDestroyAllocator(m_allocator);
			m_allocator = VK_NULL_HANDLE;
		}

#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		if (m_tracyVkCtx)
		{
			gpu::GpuProfiler::Get().Shutdown();
			aether::vulkan::DestroyTracyGpuProfilerContext(m_tracyProfilerHandle);
			m_tracyProfilerHandle = nullptr;
			TracyVkDestroy(m_tracyVkCtx);
			m_tracyVkCtx = nullptr;
		}
#endif

		vkutil::SetObjectNameFunction(nullptr);
		gpu::CommandList::SetDebugLabelFunctions(nullptr, nullptr);

		if (m_device.has_value())
		{
			vkb::destroy_device(*m_device);
		}

		if (m_debugMessenger != VK_NULL_HANDLE && m_instance.has_value())
		{
			vkDestroyDebugUtilsMessengerEXT(m_instance->instance, m_debugMessenger, nullptr);
			m_debugMessenger = VK_NULL_HANDLE;
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

	Expected<std::unique_ptr<VulkanContext>> VulkanContext::Create(const Window& window, const char* appName, bool enableGpuDiagnostics, bool enableValidation)
	{
		try
		{
			return std::unique_ptr<VulkanContext>(new VulkanContext(window, appName, enableGpuDiagnostics, enableValidation));
		}
		catch (const VulkanError& e)
		{
			return std::unexpected(AetherError::Vulkan(0, e.what()));
		}
		catch (const std::exception& e)
		{
			return std::unexpected(AetherError::Vulkan(0, std::format("VulkanContext initialization failed: {}", e.what())));
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

	VkPipelineCache VulkanContext::GetPipelineCache() const
	{
		return m_pipelineCache;
	}

	VkQueue VulkanContext::GetGraphicsQueue() const
	{
		return m_graphicsQueue;
	}

	Expected<void> VulkanContext::WaitIdle() const
	{
		AE_PROFILE_ZONE();
		if (m_device->device == VK_NULL_HANDLE)
		{
			return {};
		}
		const VkResult result = vkDeviceWaitIdle(m_device->device);
		if (result == VK_ERROR_DEVICE_LOST)
		{
			if (m_faultCallback != nullptr)
			{
				m_faultCallback();
			}
			else
			{
				QueryDeviceFaultInfo();
			}
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "VulkanContext: device lost (VK_ERROR_DEVICE_LOST)."));
		}
		if (result != VK_SUCCESS)
		{
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "VulkanContext: failed to wait for device idle."));
		}
		return {};
	}

	void VulkanContext::QueryDeviceFaultInfo() const
	{
		AE_PROFILE_ZONE();
		VkDeviceFaultCountsEXT counts{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
		};
		VkResult result = vkGetDeviceFaultInfoEXT(m_device->device, &counts, nullptr);
		if (result != VK_SUCCESS)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultInfoEXT (counts) failed: VkResult={}.", static_cast<int>(result));
			return;
		}
		if (counts.addressInfoCount == 0 && counts.vendorInfoCount == 0)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultInfoEXT: no fault records available.");
			return;
		}

		std::vector<VkDeviceFaultAddressInfoEXT> addrInfos(counts.addressInfoCount);
		std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(counts.vendorInfoCount);
		VkDeviceFaultInfoEXT info{
		        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
		        .pAddressInfos = addrInfos.data(),
		        .pVendorInfos = vendorInfos.data(),
		        .pVendorBinaryData = nullptr,
		};
		result = vkGetDeviceFaultInfoEXT(m_device->device, &counts, &info);
		if (result != VK_SUCCESS)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultInfoEXT (info) failed: VkResult={}.", static_cast<int>(result));
			return;
		}

		AE_ERROR(LogCategory::Vulkan, "=================== VK_EXT_device_fault report ===================");
		AE_ERROR(LogCategory::Vulkan, "  description: {}", info.description);

		for (uint32_t i = 0; i < counts.addressInfoCount; ++i)
		{
			const auto& ai = addrInfos[i];
			const char* typeStr = [&]() -> const char*
			{
				switch (ai.addressType)
				{
					case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
						return "ReadInvalid";
					case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
						return "WriteInvalid";
					case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
						return "ExecuteInvalid";
					case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
						return "InstrPtrUnknown";
					case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
						return "InstrPtrInvalid";
					case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
						return "InstrPtrFault";
					case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
						return "None";
					// Enum sentinel (0x7FFFFFFF), never a real address type. Listed
					case VK_DEVICE_FAULT_ADDRESS_TYPE_MAX_ENUM_KHR:
					default:
						return "Unknown";
				}
			}();
			AE_ERROR(LogCategory::Vulkan, "  addressInfo[{}]: type={} reportedAddress=0x{:016X} precision={}", i, typeStr, ai.reportedAddress, ai.addressPrecision);
		}

		for (uint32_t i = 0; i < counts.vendorInfoCount; ++i)
		{
			const auto& vi = vendorInfos[i];
			AE_ERROR(LogCategory::Vulkan, "  vendorInfo[{}]: faultCode=0x{:016X} faultData=0x{:016X} description='{}'", i, vi.vendorFaultCode, vi.vendorFaultData, vi.description);
		}

		AE_ERROR(LogCategory::Vulkan, "=================== end device fault report ===================");
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

	const VkPhysicalDeviceDescriptorHeapPropertiesEXT& VulkanContext::GetDescriptorHeapProperties() const
	{
		return m_descriptorHeapProps;
	}

	void VulkanContext::SetGlobalAddressBindingTracker(GpuMemoryTracker* tracker)
	{
		g_addressBindingTracker = tracker;
	}
} // namespace aether
