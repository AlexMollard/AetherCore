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

#include <algorithm>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <cstring>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "utils/AetherExceptions.hpp"
#include "vulkan/DeviceFaultQuery.hpp"
#include "vulkan/TransferManager.hpp"
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

	// vk-bootstrap collapses every reason a device can be rejected into one
	// "no_suitable_device" error code, which is exactly the information a user on
	// unsupported hardware does NOT have. Re-walk the devices ourselves and say, per
	// device, which requirement it missed - that report is the whole bug report.
	std::string DescribeDeviceSelectionFailure(VkInstance instance,
	                                           const std::vector<const char*>& requiredExtensions,
	                                           const VkPhysicalDeviceFeatures& required10,
	                                           const VkPhysicalDeviceVulkan11Features& required11,
	                                           const VkPhysicalDeviceVulkan12Features& required12,
	                                           const VkPhysicalDeviceVulkan13Features& required13,
	                                           const VkPhysicalDeviceVulkan14Features& required14)
	{
		std::uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
		if (deviceCount == 0)
		{
			return "The Vulkan instance reports no physical devices at all. Check that a GPU driver with Vulkan support is installed.";
		}

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

		std::string report;
		for (VkPhysicalDevice device : devices)
		{
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(device, &props);

			std::vector<std::string> unmet;

			if (props.apiVersion < VK_API_VERSION_1_4)
			{
				unmet.push_back(std::format("Vulkan {}.{}.{} (1.4 required)", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion)));
			}

			std::uint32_t extensionCount = 0;
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> available(extensionCount);
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data());
			for (const char* required : requiredExtensions)
			{
				const bool present = std::any_of(available.begin(), available.end(), [required](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, required) == 0; });
				if (!present)
				{
					unmet.emplace_back(required);
				}
			}

			// Only the promoted core feature structs are queried here: they are safe to
			// chain on any 1.1+ device, whereas an extension's feature struct is only
			// meaningful once the extension itself is present - and if it is missing, the
			// extension line above already named it.
			VkPhysicalDeviceVulkan14Features have14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
			VkPhysicalDeviceVulkan13Features have13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &have14};
			VkPhysicalDeviceVulkan12Features have12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &have13};
			VkPhysicalDeviceVulkan11Features have11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &have12};
			VkPhysicalDeviceFeatures2 have2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &have11};
			if (props.apiVersion >= VK_API_VERSION_1_1)
			{
				vkGetPhysicalDeviceFeatures2(device, &have2);
			}
			else
			{
				vkGetPhysicalDeviceFeatures(device, &have2.features);
			}

#define AE_REPORT_MISSING_FEATURE(requiredStruct, haveStruct, field)                \
	if ((requiredStruct).field == VK_TRUE && (haveStruct).field != VK_TRUE)         \
	{                                                                              \
		unmet.emplace_back("feature " #field);                                     \
	}

			AE_REPORT_MISSING_FEATURE(required10, have2.features, shaderInt64)
			AE_REPORT_MISSING_FEATURE(required10, have2.features, shaderInt16)
			AE_REPORT_MISSING_FEATURE(required10, have2.features, multiDrawIndirect)
			AE_REPORT_MISSING_FEATURE(required10, have2.features, drawIndirectFirstInstance)
			AE_REPORT_MISSING_FEATURE(required10, have2.features, fillModeNonSolid)
			AE_REPORT_MISSING_FEATURE(required10, have2.features, wideLines)

			AE_REPORT_MISSING_FEATURE(required11, have11, shaderDrawParameters)

			AE_REPORT_MISSING_FEATURE(required12, have12, bufferDeviceAddress)
			AE_REPORT_MISSING_FEATURE(required12, have12, descriptorIndexing)
			AE_REPORT_MISSING_FEATURE(required12, have12, scalarBlockLayout)
			AE_REPORT_MISSING_FEATURE(required12, have12, runtimeDescriptorArray)
			AE_REPORT_MISSING_FEATURE(required12, have12, descriptorBindingPartiallyBound)
			AE_REPORT_MISSING_FEATURE(required12, have12, descriptorBindingVariableDescriptorCount)
			AE_REPORT_MISSING_FEATURE(required12, have12, descriptorBindingSampledImageUpdateAfterBind)
			AE_REPORT_MISSING_FEATURE(required12, have12, shaderSampledImageArrayNonUniformIndexing)
			AE_REPORT_MISSING_FEATURE(required12, have12, timelineSemaphore)
			AE_REPORT_MISSING_FEATURE(required12, have12, drawIndirectCount)
			AE_REPORT_MISSING_FEATURE(required12, have12, shaderInt8)
			AE_REPORT_MISSING_FEATURE(required12, have12, hostQueryReset)

			AE_REPORT_MISSING_FEATURE(required13, have13, dynamicRendering)
			AE_REPORT_MISSING_FEATURE(required13, have13, synchronization2)

			AE_REPORT_MISSING_FEATURE(required14, have14, hostImageCopy)
			AE_REPORT_MISSING_FEATURE(required14, have14, pushDescriptor)

#undef AE_REPORT_MISSING_FEATURE

			std::string joined;
			for (const std::string& item : unmet)
			{
				if (!joined.empty())
				{
					joined += ", ";
				}
				joined += item;
			}

			// driverVersion is vendor-encoded, not a Vulkan version, so it is reported raw
			// rather than decoded into a major.minor.patch that would read as a lie.
			report += std::format("\n  - '{}' (vendorID=0x{:04X}, deviceID=0x{:04X}, apiVersion {}.{}.{}, driverVersion 0x{:08X}): {}",
			                      props.deviceName,
			                      props.vendorID,
			                      props.deviceID,
			                      VK_API_VERSION_MAJOR(props.apiVersion),
			                      VK_API_VERSION_MINOR(props.apiVersion),
			                      VK_API_VERSION_PATCH(props.apiVersion),
			                      props.driverVersion,
			                      unmet.empty() ? std::string("meets every version/extension/core-feature requirement - rejected on a surface, queue-family or extension-feature requirement instead")
			                                    : std::format("missing {}", joined));
		}

		return report;
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

		// Whether the layer is going to be loaded on THIS run. Compiled out entirely for a
		// shipped build; otherwise it is whatever the config resolved to.
		//
		// Everything downstream must key off this rather than off VK_VALIDATION_CPU. The
		// two used to be treated as the same thing, which was harmless while validation was
		// on for every dev build and became wrong the moment --no-validation existed: see
		// the Aftermath gate below, which used to refuse to start because the layer was
		// *compiled in* even on runs where it was switched off.
#if VK_VALIDATION_CPU
		const bool validationActive = enableValidation;
#else
		constexpr bool validationActive = false;
#endif

		vkb::InstanceBuilder instanceBuilder;
		instanceBuilder.set_app_name(appName);
		instanceBuilder.require_api_version(1, 4, 0);
#if VK_VALIDATION_CPU
		// "Is validation on?" is build-config dependent now - on for Debug, off for
		// RelWithDebInfo - so the answer and its provenance go in the log unambiguously.
		// A session that is mysteriously slow and one that is mysteriously uninstrumented
		// look identical until you can read this line.
		AE_INFO(LogCategory::Vulkan,
		        "Vulkan validation layer: {} for this run ({} build defaults to {}; --validation and --no-validation override, --no-validation wins if both are passed).",
		        validationActive ? "ENABLED" : "DISABLED",
		        AE_CONFIG_NAME,
		        AE_VALIDATION_DEFAULT_ON != 0 ? "enabled" : "disabled");

		// The layer accumulates state (~2-3 KB/frame) and progressively slows the render
		// thread over long sessions, which is why it is not the RelWithDebInfo default.
		if (validationActive)
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
			AE_INFO(LogCategory::Vulkan, "Vulkan validation layer not loaded - stable frame rate, no validation diagnostics. NVIDIA Aftermath GPU crash dumps are available instead.");
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
		const VkPhysicalDeviceVulkan14Features requiredFeatures14{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
		        .hostImageCopy = VK_TRUE,
		        .pushDescriptor = VK_TRUE,
		};
		selector.set_required_features_14(requiredFeatures14);

		// Held as a list rather than fed straight into the selector so the failure path
		// can diff it against each rejected device and name what is actually missing.
		std::vector<const char*> requiredExtensions;
#ifdef TRACY_ENABLE
		// VK_EXT_calibrated_timestamps is required for Tracy host-calibrated GPU zones.
		requiredExtensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif
		// Only what the renderer genuinely cannot draw without belongs here. VK_KHR_push_descriptor
		// is deliberately absent: it is core in 1.4 and the pushDescriptor feature bit above already
		// covers it, so naming it again only narrowed the set of devices that could pass selection.
		//
		// VK_EXT_shader_object: layout-free shaders bound directly via vkCmdBindShadersEXT.
		requiredExtensions.push_back(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
		// an extension and is required explicitly below because shader objects use EDS3
		requiredExtensions.push_back(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
		requiredExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
		// picks one. Requesting them as *required* selector extensions this
		for (const char* extension : requiredExtensions)
		{
			selector.add_required_extension(extension);
		}

		auto physicalDeviceResult = selector.select();

		if (!physicalDeviceResult)
		{
			// "Failed to select a suitable Vulkan physical device." on its own is the
			// least actionable message the engine can produce: it fires on every
			// unsupported GPU and says nothing about which requirement was missed. The
			// per-device breakdown below is what a user on other hardware has to send back.
			Throw(AetherError::Vulkan(0,
			                          std::format("Failed to select a suitable Vulkan physical device: {} ({}).\nRequirements checked per device:{}",
			                                      physicalDeviceResult.error().message(),
			                                      physicalDeviceResult.error().value(),
			                                      DescribeDeviceSelectionFailure(m_instance->instance, requiredExtensions, requiredFeatures10, requiredFeatures11, requiredFeatures12, requiredFeatures13, requiredFeatures14))));
		}

		// gated on two independent conditions, both required:
#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH
		bool aftermathDeviceExtensionsEnabled = false;
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
			// Aftermath and the validation layer are mutually exclusive, so this turns on
			// exactly when the layer stayed off. Note the RUNTIME flag: keying this off
			// VK_VALIDATION_CPU meant a --no-validation run reported "disabled because the
			// validation layer is active" while running with no validation layer at all,
			// and shipped no crash dumps for the one configuration that most needed them.
			else if (validationActive)
			{
				AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: disabled because the Vulkan validation layer is active this run (mutually incompatible; pass --no-validation to get GPU crash dumps instead).");
			}
			else
			{
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
			}
		}
#endif

		// Enabled where present, skipped where not. None of these three change a single pixel:
		// maintenance9 is chained but nothing in the engine depends on it, and device_fault and
		// device_address_binding_report only feed post-mortem diagnostics that already null-check
		// their entry points. They were hard selection requirements, which meant a GPU perfectly
		// capable of running the renderer was rejected outright for missing a debugging aid -
		// and device_address_binding_report in particular is close to NVIDIA-only.
		const bool maintenance9Present = physicalDeviceResult.value().enable_extension_if_present(VK_KHR_MAINTENANCE_9_EXTENSION_NAME);
		const bool deviceFaultPresent = physicalDeviceResult.value().enable_extension_if_present(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
		const bool addressBindingReportPresent = physicalDeviceResult.value().enable_extension_if_present(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
		AE_INFO(LogCategory::Vulkan,
		        "Optional device extensions: VK_KHR_maintenance9={}, VK_EXT_device_fault={} (GPU fault reports), VK_EXT_device_address_binding_report={} (allocation tracking).",
		        maintenance9Present,
		        deviceFaultPresent,
		        addressBindingReportPresent);

		// Optional present-timing pair, enabled only where the driver has both. Pacing a
		// frame against a GUESSED vsync phase is worse than not pacing it - a mistimed frame
		// is a dropped frame - so the feature stays off unless the real timebase exists.
		const bool presentIdPresent = physicalDeviceResult.value().enable_extension_if_present(VK_KHR_PRESENT_ID_EXTENSION_NAME);
		const bool presentWaitPresent = presentIdPresent && physicalDeviceResult.value().enable_extension_if_present(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
		m_presentTimingSupported = presentIdPresent && presentWaitPresent;
		AE_INFO(LogCategory::Vulkan, "Present timing (VK_KHR_present_id/present_wait): {} (present_id={}, present_wait={}).", m_presentTimingSupported ? "available" : "unavailable", presentIdPresent, presentWaitPresent);

		VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		        .presentId = VK_TRUE,
		};
		VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures{
		        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
		        .presentWait = VK_TRUE,
		};

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
		// A feature struct may only be chained when its extension was actually enabled;
		// chaining one for an absent extension fails device creation outright.
		if (maintenance9Present)
		{
			deviceBuilder.add_pNext(&maintenance9Features);
		}
		deviceBuilder.add_pNext(&shaderObjectFeatures);
		deviceBuilder.add_pNext(&descriptorHeapFeatures);
		if (deviceFaultPresent)
		{
			deviceBuilder.add_pNext(&faultFeatures);
		}
		if (addressBindingReportPresent)
		{
			deviceBuilder.add_pNext(&addressBindingReportFeatures);
		}
		deviceBuilder.add_pNext(&extendedDynamicStateFeatures);
		if (m_presentTimingSupported)
		{
			deviceBuilder.add_pNext(&presentIdFeatures);
			deviceBuilder.add_pNext(&presentWaitFeatures);
		}
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

		// Upload queue: prefer the dedicated DMA family (transfer-only), then any
		// separate transfer-capable family, and fall back to the graphics queue when
		// the hardware exposes neither (TransferManager stays correct either way).
		m_transferQueue = m_graphicsQueue;
		m_transferQueueFamily = m_graphicsQueueFamily;
		if (const auto dedicatedTransfer = m_device->get_dedicated_queue(vkb::QueueType::transfer))
		{
			m_transferQueue = dedicatedTransfer.value();
			m_transferQueueFamily = m_device->get_dedicated_queue_index(vkb::QueueType::transfer).value();
		}
		else if (const auto separateTransfer = m_device->get_queue(vkb::QueueType::transfer))
		{
			m_transferQueue = separateTransfer.value();
			if (const auto transferIndex = m_device->get_queue_index(vkb::QueueType::transfer))
			{
				m_transferQueueFamily = transferIndex.value();
			}
		}

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
		if (m_transferQueue != m_graphicsQueue && m_transferQueue != m_computeQueue && m_transferQueue != m_presentQueue)
		{
			vkutil::SetObjectName(m_device->device, reinterpret_cast<std::uint64_t>(static_cast<void*>(m_transferQueue)), VK_OBJECT_TYPE_QUEUE, "Queue.Transfer");
		}

		// Host image copy: cache which layouts the implementation lets the host
		// transition to, so texture uploads can finish entirely on the CPU (no queue
		// submission) when SHADER_READ_ONLY_OPTIMAL is supported.
		{
			VkPhysicalDeviceHostImageCopyProperties hostCopyProps{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES};
			VkPhysicalDeviceProperties2 props2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hostCopyProps};
			vkGetPhysicalDeviceProperties2(m_device->physical_device, &props2);
			std::vector<VkImageLayout> dstLayouts(hostCopyProps.copyDstLayoutCount);
			hostCopyProps.pCopyDstLayouts = dstLayouts.data();
			hostCopyProps.copySrcLayoutCount = 0;
			hostCopyProps.pCopySrcLayouts = nullptr;
			vkGetPhysicalDeviceProperties2(m_device->physical_device, &props2);
			vkutil::SetHostImageCopyDstLayouts(std::move(dstLayouts));
		}

		m_transferManager = std::make_unique<vulkan::TransferManager>();
		m_transferManager->Initialize(m_device->device, m_transferQueue, m_transferQueueFamily, m_graphicsQueueFamily);

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

		// Waits the transfer timeline idle and reclaims staging; must precede device destroy.
		if (m_transferManager != nullptr)
		{
			m_transferManager->Shutdown();
			m_transferManager.reset();
		}
		vkutil::SetHostImageCopyDstLayouts({});

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
		// One implementation of the query lives in DeviceFaultQuery; this is the fallback
		// path used when no DiagnosticEngine has registered a fault callback, so it just
		// dumps the record. Both the device handle and the entry point are checked there.
		const VkDevice device = m_device.has_value() ? m_device->device : VK_NULL_HANDLE;
		vulkan::LogDeviceFaultReport(vulkan::QueryDeviceFault(vkGetDeviceFaultInfoEXT, device));
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

	VkQueue VulkanContext::GetTransferQueue() const
	{
		return m_transferQueue;
	}

	std::uint32_t VulkanContext::GetTransferQueueFamily() const
	{
		return m_transferQueueFamily;
	}

	vulkan::TransferManager& VulkanContext::GetTransferManager() const
	{
		AE_ASSERT(m_transferManager != nullptr, "VulkanContext: transfer manager not initialised");
		return *m_transferManager;
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
