#include "vulkan/VulkanContext.hpp"

#include <format>
#include <fstream>
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
#ifdef TRACY_ENABLE
#	include <tracy/TracyVulkan.hpp>
#endif
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/GpuMemoryTracker.hpp"
#include "platform/Window.hpp"

// Validation mode is controlled by CMake options AETHERCORE_VULKAN_GPU_DEBUG /
// AETHERCORE_VULKAN_CPU_DEBUG (see CMake/TargetDefaults.cmake). The resulting
// VULKAN_GPU_DEBUG / VULKAN_CPU_DEBUG macros are passed as compile definitions.
#if defined(VULKAN_GPU_DEBUG) && defined(VULKAN_CPU_DEBUG)
#	error "VULKAN_GPU_DEBUG and VULKAN_CPU_DEBUG are mutually exclusive"
#endif

namespace
{
	// Set by GraphicsDevice::Init so the VK_EXT_device_address_binding_report
	// events delivered through the debug messenger can register/unregister GPU
	// memory ranges in a single place (including driver-internal allocations).
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

	VKAPI_ATTR VkBool32 VKAPI_CALL LogValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
	{
		(void) userData;

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

		// Walk the pNext chain to surface structured diagnostic data that the
		// driver / layers attach, notably VK_EXT_device_address_binding_report
		// callbacks (VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_REPORT_CALLBACK_DATA_EXT)
		// which report every BDA bind/unbind for resource-leak tracking.
		std::string extra;
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
		else
		{
			aether::Logger::VerboseAt(aether::LogCategory::Validation, std::source_location::current(), "{}", decorated);
		}

		return VK_FALSE;
	}
} // namespace

namespace aether
{
	VulkanContext::VulkanContext(const Window& window, const char* appName)
	{
		AE_INFO(LogCategory::Vulkan, "Creating Vulkan context for '{}'.", appName);

		if (volkInitialize() != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to initialize volk Vulkan loader."));
		}

		vkb::InstanceBuilder instanceBuilder;
		instanceBuilder.set_app_name(appName);
		instanceBuilder.require_api_version(1, 4, 0);
#if defined(VULKAN_GPU_DEBUG) || defined(VULKAN_CPU_DEBUG)
		VkDebugUtilsMessageSeverityFlagsEXT debugSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		VkDebugUtilsMessageTypeFlagsEXT debugTypes = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		VkDebugUtilsMessageTypeFlagsEXT debugTypesWithAddressBinding = debugTypes | VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
		debugSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
		instanceBuilder.request_validation_layers();
		instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
#if defined(VULKAN_GPU_DEBUG)
		instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
		instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
		// Debug printf routes shader debugPrintfEXT() calls through the debug
		// messenger so GPU-side printfs surface in the log.
		instanceBuilder.add_validation_feature_enable(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
		instanceBuilder.add_validation_feature_disable(VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT);
		AE_INFO(LogCategory::Vulkan, "GPU-AV + Synchronization Validation + Debug Printf enabled.");
#elif defined(VULKAN_CPU_DEBUG)
		AE_INFO(LogCategory::Vulkan, "Core Validation (CPU) enabled.");
#endif

#if defined(VULKAN_GPU_DEBUG)
		// VK_LAYER_LUNARG_crash_diagnostic captures command buffer state at GPU
		// hang time, producing a dump that identifies the exact draw/dispatch
		// that faulted. Enumerate available instance layers and enable it only
		// if present so instance creation does not fail on systems without it.
		{
			uint32_t layerCount = 0;
			vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> layers(layerCount);
			vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
			for (const auto& layer: layers)
			{
				if (std::strcmp(layer.layerName, "VK_LAYER_LUNARG_crash_diagnostic") == 0)
				{
					instanceBuilder.enable_layer("VK_LAYER_LUNARG_crash_diagnostic");
					AE_INFO(LogCategory::Vulkan, "Crash diagnostic layer enabled.");
					break;
				}
			}
		}
#endif

		auto instanceResult = instanceBuilder.build();

		if (!instanceResult)
		{
			Throw(AetherError::Vulkan(0, std::string("Failed to create Vulkan instance: ") + instanceResult.error().message()));
		}

		m_instance = instanceResult.value();

		volkLoadInstance(m_instance->instance);

#if defined(VULKAN_GPU_DEBUG) || defined(VULKAN_CPU_DEBUG)
		VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo{
		        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		        .messageSeverity = debugSeverity,
		        .messageType = debugTypes,
		        .pfnUserCallback = LogValidationMessage,
		};
		VkResult messengerResult = vkCreateDebugUtilsMessengerEXT(m_instance->instance, &debugMessengerCreateInfo, nullptr, &m_debugMessenger);
		if (messengerResult != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(messengerResult, "Failed to create Vulkan debug messenger."));
		}
#endif

		if (glfwCreateWindowSurface(m_instance->instance, window.GetHandle(), nullptr, &m_surface) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "Failed to create Vulkan surface."));
		}

		VkPhysicalDeviceVulkan11Features requiredFeatures11{};
		requiredFeatures11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		// shaderDrawParameters is core in 1.1+ but kept for SDK / pre-1.4 validation compatibility.
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
		requiredFeatures10.fillModeNonSolid = VK_TRUE;
		requiredFeatures10.wideLines = VK_TRUE;

		vkb::PhysicalDeviceSelector selector{*m_instance};
		selector.set_surface(m_surface).set_minimum_version(1, 4).set_required_features(requiredFeatures10).set_required_features_11(requiredFeatures11).set_required_features_12(requiredFeatures12).set_required_features_13(requiredFeatures13);
		{
			VkPhysicalDeviceVulkan14Features features14{
			        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
			        .hostImageCopy = VK_TRUE,
			        .pushDescriptor = VK_TRUE,
			};
			selector.set_required_features_14(features14);
		}
#ifdef TRACY_ENABLE
		// VK_EXT_calibrated_timestamps is required for Tracy host-calibrated GPU zones.
		// It is promoted to core in Vulkan 1.4 under the KHR name, but we request the EXT
		// extension explicitly so vkb enables it and the function pointers are available.
		selector.add_required_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif
		// VK_KHR_maintenance9: optional device extension. Required by this renderer
		// for compatible queue-family ownership transfer behavior.
		selector.add_required_extension(VK_KHR_MAINTENANCE_9_EXTENSION_NAME);
		// Push descriptors are represented in Vulkan 1.4 core structures, but
		// requesting the legacy extension keeps extension-suffixed entry points
		// and driver paths explicit for this renderer.
		selector.add_required_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
		// VK_EXT_shader_object: layout-free shaders bound directly via vkCmdBindShadersEXT.
		// Still a device extension, not Vulkan 1.4 core. Required explicitly.
		selector.add_required_extension(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
		// Extended dynamic state v1/v2 functionality is available through Vulkan 1.3/core
		// entry points for the states used here. VK_EXT_extended_dynamic_state3 is still
		// an extension and is required explicitly below because shader objects use EDS3
		// rasterization and blend dynamic states.
		// VK_EXT_descriptor_heap: explicit descriptor memory management using one
		// resource heap and one sampler heap. This is an extension, not Vulkan 1.4 core.
		// It can replace descriptor sets/pipeline layouts for heap-based binding while
		// still allowing set/binding shader decorations to map to heap offsets.
		selector.add_required_extension(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		selector.add_required_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
		// VK_KHR_device_fault: on device loss, vkGetDeviceFaultReportsKHR returns
		// detailed fault addresses (memory + instruction), vendor-specific data,
		// and a description string. deviceFaultDeviceLostOnMasked forces the
		// driver to surface soft (masked) hardware errors as hard device-lost
		// events so they are not silently absorbed. Near-zero cost when no fault
		// occurs - enabled unconditionally.
		selector.add_required_extension(VK_KHR_DEVICE_FAULT_EXTENSION_NAME);
		// VK_EXT_device_address_binding_report: the driver reports every bind/
		// unbind of a device address range through the debug messenger pNext
		// chain (VkDeviceAddressBindingCallbackDataEXT). Used by the
		// DiagnosticEngine to maintain a BDA -> resource-name registry for
		// post-mortem address resolution.
		selector.add_required_extension(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
		// VK_NV_device_diagnostic_checkpoints: driver-stored breadcrumb markers
		// for post-crash flight-recorder dump. vkCmdSetCheckpointNV writes a
		// marker into the command stream; vkGetQueueCheckpointDataNV retrieves
		// the last N markers after device loss. The driver retains checkpoints
		// internally so they survive device loss even when mapped memory does
		// not. Compared to VK_AMD_buffer_marker, this is Nvidia-specific but
		// strictly more resilient (driver-stored, not host-visible-buffer).
		// Works alongside VK_AMD_buffer_marker; DiagnosticEngine writes markers
		// via whichever mechanism is available.
		selector.add_required_extension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		// VK_NV_device_diagnostics_config is required for Aftermath resource tracking
		// and shader debug info. If unavailable (non-NVIDIA GPU), device selection will fail.
		// VK_NV_device_diagnostic_checkpoints provides vkCmdSetCheckpointNV for event markers.
		{
			[[maybe_unused]] auto _ = m_aftermathContext.EnableGpuCrashDumps(".");
			selector.add_required_extension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
			selector.add_required_extension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
		}
#endif

		auto physicalDeviceResult = selector.select();

		if (!physicalDeviceResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to select a suitable Vulkan physical device."));
		}

		// Non-core extension feature structs chained into the vkb::DeviceBuilder
		// pNext. The core 1.1/1.2/1.3/1.4 features above are handled by vkb
		// internally; only the hardware-specific extension features need to be
		// chained here. vkb owns the lifetime of the core feature structs it
		// copied during select(), so these locals only need to outlive build().
		// Each struct is added via its own add_pNext call: vkb builds the pNext
		// chain internally by overwriting each struct's pNext field, so a
		// manual chain must NOT be set up here.
		VkPhysicalDeviceMaintenance9FeaturesKHR maintenance9Features{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR,
		        .maintenance9 = VK_TRUE,
		};

		VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
		        .shaderObject = VK_TRUE,
		};

		VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
		        .descriptorHeap = VK_TRUE,
		};

		// Query supported fault features on this physical device so we only
		// request what the hardware actually supports. deviceFaultReportMasked
		// and deviceFaultDeviceLostOnMasked are optional - NVIDIA beta drivers
		// (and some production drivers) may not support them.
		VkPhysicalDeviceFaultFeaturesKHR supportedFaultFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR,
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
		                                 && supportedEDS3.extendedDynamicState3AlphaToCoverageEnable == VK_TRUE && supportedEDS3.extendedDynamicState3AlphaToOneEnable == VK_TRUE && supportedEDS3.extendedDynamicState3LogicOpEnable == VK_TRUE
		                                 && supportedEDS3.extendedDynamicState3ColorBlendEnable == VK_TRUE && supportedEDS3.extendedDynamicState3ColorBlendEquation == VK_TRUE && supportedEDS3.extendedDynamicState3ColorWriteMask == VK_TRUE;
		if (!eds3RequiredSupport)
		{
			Throw(AetherError::Vulkan(0, "VK_EXT_extended_dynamic_state3 is missing required dynamic state features."));
		}

		VkPhysicalDeviceFaultFeaturesKHR faultFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR,
		        .deviceFault = supportedFaultFeatures.deviceFault,
		        .deviceFaultVendorBinary = supportedFaultFeatures.deviceFaultVendorBinary,
		        .deviceFaultReportMasked = supportedFaultFeatures.deviceFaultReportMasked,
		        .deviceFaultDeviceLostOnMasked = supportedFaultFeatures.deviceFaultDeviceLostOnMasked,
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
		deviceBuilder.add_pNext(&diagnosticsConfig);
#endif
		auto deviceResult = deviceBuilder.build();
		if (!deviceResult)
		{
			Throw(AetherError::Vulkan(0, "Failed to create Vulkan logical device."));
		}

		m_device = deviceResult.value();

		volkLoadDevice(m_device->device);

#if defined(VULKAN_GPU_DEBUG) || defined(VULKAN_CPU_DEBUG)
		VkDebugUtilsMessengerEXT upgradedDebugMessenger = VK_NULL_HANDLE;
		VkDebugUtilsMessengerCreateInfoEXT upgradedDebugMessengerCreateInfo{
		        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		        .messageSeverity = debugSeverity,
		        .messageType = debugTypesWithAddressBinding,
		        .pfnUserCallback = LogValidationMessage,
		};
		VkResult upgradedMessengerResult = vkCreateDebugUtilsMessengerEXT(m_instance->instance, &upgradedDebugMessengerCreateInfo, nullptr, &upgradedDebugMessenger);
		if (upgradedMessengerResult == VK_SUCCESS)
		{
			vkDestroyDebugUtilsMessengerEXT(m_instance->instance, m_debugMessenger, nullptr);
			m_debugMessenger = upgradedDebugMessenger;
		}
		else
		{
			AE_WARN(LogCategory::Vulkan, "Unable to enable device address binding debug messenger events after Vulkan device creation: VkResult={}.", static_cast<int>(upgradedMessengerResult));
		}
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

		const auto setObjectNameFn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(vkGetDeviceProcAddr(m_device->device, "vkSetDebugUtilsObjectNameEXT"));
		vkutil::SetObjectNameFunction(setObjectNameFn);

		// Graphics and compute may be the same queue on some hardware; guard against double-naming.
		vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_graphicsQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Graphics");
		if (m_computeQueue != m_graphicsQueue)
		{
			vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_computeQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Compute");
		}
		if (m_presentQueue != m_graphicsQueue && m_presentQueue != m_computeQueue)
		{
			vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_presentQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Present");
		}

		// Validate push constant size against hardware limits. Vulkan 1.0 guarantees
		// at least 128 bytes, but explicit verification catches drivers that may
		// report less for unusual virtualized/adapter configurations.
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
		}

		// Query VK_EXT_descriptor_heap properties. Used by BindlessManager to
		// size/align the resource and sampler heap backing buffers.
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

		// Pipeline cache for faster pipeline creation across runs.
		// Attempt to load cached data from a previous session; fall back to empty
		// if the file is missing or the driver rejects the data (e.g. after a
		// driver update where the cache UUID no longer matches).
		{
			std::vector<char> cacheData;
			if (std::ifstream inFile("pipeline_cache.bin", std::ios::binary | std::ios::ate); inFile.is_open())
			{
				const auto fileSize = inFile.tellg();
				if (fileSize > 0)
				{
					cacheData.resize(static_cast<std::size_t>(fileSize));
					inFile.seekg(0);
					inFile.read(cacheData.data(), fileSize);
				}
				inFile.close();
				AE_INFO(LogCategory::Vulkan, "Loaded pipeline cache from pipeline_cache.bin ({} bytes).", cacheData.size());
			}

			const VkPipelineCacheCreateInfo cacheInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
			        .initialDataSize = cacheData.size(),
			        .pInitialData = cacheData.empty() ? nullptr : cacheData.data(),
			};
			VkResult cacheResult = vkCreatePipelineCache(m_device->device, &cacheInfo, nullptr, &m_pipelineCache);
			if (cacheResult != VK_SUCCESS && cacheResult != VK_ERROR_OUT_OF_HOST_MEMORY)
			{
				// Driver rejected cached data (driver update, device mismatch, etc.).
				// Retry with an empty cache.
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

		// Report GPU (VkDeviceMemory) allocations to Tracy as the "GPU" named pool
		// and optionally to GpuMemoryTracker for address resolution.
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
			// Build the engine-side pImpl via the vulkan-side factory
			// and hand it to the engine singleton. Engine code never
			// sees `tracy::VkCtx*` or any other `Vk*`-named type.
			m_tracyProfilerHandle = aether::vulkan::CreateTracyGpuProfilerContext(m_tracyVkCtx);
			gpu::GpuProfiler::Get().Initialize({m_tracyProfilerHandle});
			gpu::GpuProfiler::Get().SetName("AetherCore GPU");
		}
#endif

#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		[[maybe_unused]] auto _ = m_aftermathContext.Initialize(m_device->device, physicalDeviceResult.value().physical_device);
#endif

		AE_INFO(LogCategory::Vulkan, "Vulkan context initialized successfully.");
	}

	VulkanContext::~VulkanContext()
	{
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
					if (saveResult == VK_SUCCESS || saveResult == VK_INCOMPLETE)
					{
						if (std::ofstream outFile("pipeline_cache.bin", std::ios::binary); outFile.is_open())
						{
							outFile.write(cacheData.data(), static_cast<std::streamsize>(cacheSize));
							outFile.close();
							AE_INFO(LogCategory::Vulkan, "Saved pipeline cache to pipeline_cache.bin ({} bytes).", cacheSize);
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

#ifdef TRACY_ENABLE
		if (m_tracyVkCtx)
		{
			// Drop the engine-side reference before destroying the
			// Tracy context so any in-flight GpuZoneScope sees a
			// null context and becomes a no-op.
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

	void VulkanContext::WaitIdle() const
	{
		if (m_device->device == VK_NULL_HANDLE)
		{
			return;
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
			Throw(AetherError::Vulkan(static_cast<int32_t>(result), "VulkanContext: device lost (VK_ERROR_DEVICE_LOST)."));
		}
		if (result != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(static_cast<int32_t>(result), "VulkanContext: failed to wait for device idle."));
		}
	}

	void VulkanContext::QueryDeviceFaultInfo() const
	{
		const auto vkGetDeviceFaultReportsKHR = reinterpret_cast<PFN_vkGetDeviceFaultReportsKHR>(vkGetDeviceProcAddr(m_device->device, "vkGetDeviceFaultReportsKHR"));
		if (vkGetDeviceFaultReportsKHR == nullptr)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultReportsKHR not available; cannot query device fault info.");
			return;
		}

		// Pass 1: query the number of fault records the driver has preserved.
		// timeout=0 returns immediately with whatever is currently available.
		uint32_t faultCount = 0;
		VkResult result = vkGetDeviceFaultReportsKHR(m_device->device, 0, &faultCount, nullptr);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultReportsKHR (count) failed: VkResult={}.", static_cast<int>(result));
			return;
		}
		if (faultCount == 0)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultReportsKHR: driver preserved no hardware diagnostic records.");
			return;
		}

		// Pass 2: allocate fault records and fetch. Each VkDeviceFaultInfoKHR
		// carries inline address/vendor info (not pointer arrays like the EXT
		// variant).
		std::vector<VkDeviceFaultInfoKHR> faults(faultCount);
		for (uint32_t i = 0; i < faultCount; ++i)
		{
			faults[i].sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR;
		}

		result = vkGetDeviceFaultReportsKHR(m_device->device, 0, &faultCount, faults.data());
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		{
			AE_WARN(LogCategory::Vulkan, "vkGetDeviceFaultReportsKHR (info) failed: VkResult={}.", static_cast<int>(result));
			return;
		}

		AE_ERROR(LogCategory::Vulkan, "=================== VK_KHR_device_fault report ({} fault(s)) ===================", faultCount);
		for (uint32_t i = 0; i < faultCount; ++i)
		{
			const auto& f = faults[i];

			// Decode fault flag bits into human-readable causes.
			std::string flagStr;
			if (f.flags == 0)
			{
				flagStr = "(none)";
			}
			else
			{
				if (f.flags & VK_DEVICE_FAULT_FLAG_DEVICE_LOST_KHR)
				{
					flagStr += "DeviceLost|";
				}
				if (f.flags & VK_DEVICE_FAULT_FLAG_MEMORY_ADDRESS_KHR)
				{
					flagStr += "MemoryAddress|";
				}
				if (f.flags & VK_DEVICE_FAULT_FLAG_INSTRUCTION_ADDRESS_KHR)
				{
					flagStr += "InstructionAddress|";
				}
				if (f.flags & VK_DEVICE_FAULT_FLAG_VENDOR_KHR)
				{
					flagStr += "Vendor|";
				}
				if (f.flags & VK_DEVICE_FAULT_FLAG_WATCHDOG_TIMEOUT_KHR)
				{
					flagStr += "WatchdogTimeout(TDR)|";
				}
				if (f.flags & VK_DEVICE_FAULT_FLAG_OVERFLOW_KHR)
				{
					flagStr += "Overflow|";
				}
				if (!flagStr.empty() && flagStr.back() == '|')
				{
					flagStr.pop_back();
				}
			}

			AE_ERROR(LogCategory::Vulkan, "----- Fault[{}] groupId={} flags=0x{:X} ({}) -----", i, f.groupId, static_cast<uint32_t>(f.flags), flagStr);
			AE_ERROR(LogCategory::Vulkan, "  description: {}", f.description);

			// Memory (MMU) fault address.
			if (f.faultAddressInfo.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR)
			{
				const auto& ai = f.faultAddressInfo;
				const char* typeStr = ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_KHR                  ? "ReadInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_KHR               ? "WriteInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_KHR             ? "ExecuteInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_KHR ? "InstrPtrUnknown"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_KHR ? "InstrPtrInvalid"
				                      : ai.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_KHR   ? "InstrPtrFault"
				                                                                                                       : "Unknown";
				AE_ERROR(LogCategory::Vulkan, "  faultAddress: type={} reportedAddress=0x{:016X} precision={}", typeStr, ai.reportedAddress, ai.addressPrecision);
			}

			// Instruction pointer at the time of the fault - used with
			// VK_KHR_pipeline_executable_properties / shader debug info to
			// resolve to a SPIR-V/Slang source line.
			if (f.instructionAddressInfo.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR)
			{
				const auto& ii = f.instructionAddressInfo;
				AE_ERROR(LogCategory::Vulkan, "  instructionAddress: type={} reportedAddress=0x{:016X} precision={}", static_cast<int>(ii.addressType), ii.reportedAddress, ii.addressPrecision);
			}

			// Vendor-specific fault code/data.
			if (f.vendorInfo.description[0] != '\0')
			{
				const auto& vi = f.vendorInfo;
				AE_ERROR(LogCategory::Vulkan, "  vendor: faultCode=0x{:016X} faultData=0x{:016X} description='{}'", vi.vendorFaultCode, vi.vendorFaultData, vi.description);
			}
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
