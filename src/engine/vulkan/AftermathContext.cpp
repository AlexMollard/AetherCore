#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH

#include "vulkan/AftermathContext.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		std::filesystem::path BuildDumpPath(const std::filesystem::path& baseDir, const std::string& prefix, const std::string& extension)
		{
			const auto now = std::chrono::system_clock::now();
			const auto timeT = std::chrono::system_clock::to_time_t(now);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

			tm timeInfo;
#if defined(_MSC_VER)
			localtime_s(&timeInfo, &timeT);
#else
			localtime_r(&timeT, &timeInfo);
#endif

			char timeBuf[64];
			std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &timeInfo);

			char filename[256];
			std::snprintf(filename, sizeof(filename), "%s%s_%03d%s", prefix.c_str(), timeBuf, static_cast<int>(ms.count()), extension.c_str());

			return baseDir / filename;
		}

		void WriteRawDumpToDisk(const std::filesystem::path& baseDir, const void* data, std::uint32_t size, const std::string& prefix, const std::string& extension)
		{
			auto path = BuildDumpPath(baseDir, prefix, extension);

			std::ofstream out(path, std::ios::binary);
			out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			out.close();

			AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: {} written to {} ({} bytes)", prefix, path.string(), size);
		}

		void WriteCrashDumpToDisk(const std::string& dir, const void* data, std::uint32_t size)
		{
			try
			{
				std::filesystem::path baseDir = dir.empty() ? std::filesystem::current_path() / "gpu_crash_dumps" : std::filesystem::path(dir);
				std::filesystem::create_directories(baseDir);

				auto path = BuildDumpPath(baseDir, "crash", ".nv-gpudmp");

				std::ofstream out(path, std::ios::binary);
				out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
				out.close();

				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dump written to {}", path.string());

				GFSDK_Aftermath_GpuCrashDump_Decoder decoder{};
				GFSDK_Aftermath_Result decResult = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
				        GFSDK_Aftermath_Version_API,
				        data,
				        size,
				        &decoder);
				if (decResult == GFSDK_Aftermath_Result_Success)
				{
					const std::uint32_t decFlags = 0x3EFFu; // ALL_INFO minus SHADER_MAPPING_INFO
					const std::uint32_t jsonFlags = 0u;
					std::uint32_t jsonSize = 0;
					decResult = GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
					        decoder, decFlags, jsonFlags, nullptr, nullptr, nullptr, nullptr, &jsonSize);
					if (decResult == GFSDK_Aftermath_Result_Success && jsonSize > 0)
					{
						std::vector<char> jsonBuf(jsonSize + 1);
						decResult = GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
						        decoder, decFlags, jsonFlags, nullptr, nullptr, nullptr, jsonBuf.data(), &jsonSize);
						if (decResult == GFSDK_Aftermath_Result_Success)
						{
							jsonBuf[jsonSize] = '\0';
							auto jsonPath = baseDir / (path.stem().string() + ".json");
							std::ofstream jsonOut(jsonPath, std::ios::binary);
							jsonOut.write(jsonBuf.data(), static_cast<std::streamsize>(jsonSize));
							jsonOut.close();
							AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: JSON dump written to {}", jsonPath.string());
						}
					}
					GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
				}
			}
			catch (...)
			{
				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to write GPU crash dump to disk");
			}
		}

		void WriteShaderDebugInfoToDisk(const std::filesystem::path& baseDir, const void* data, std::uint32_t size)
		{
			try
			{
				std::filesystem::create_directories(baseDir);
				WriteRawDumpToDisk(baseDir, data, size, "shader_debug", ".nvdbg");
			}
			catch (...)
			{
				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to write shader debug info to disk");
			}
		}
	} // namespace

	void GFSDK_AFTERMATH_CALL AftermathContext::OnCrashDump(const void* pGpuCrashDump, std::uint32_t gpuCrashDumpSize, void* pUserData)
	{
		AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dump received ({} bytes)", gpuCrashDumpSize);
		auto* self = static_cast<AftermathContext*>(pUserData);
		WriteCrashDumpToDisk(self->m_crashDumpDir, pGpuCrashDump, gpuCrashDumpSize);
	}

	void GFSDK_AFTERMATH_CALL AftermathContext::OnShaderDebugInfo(const void* pShaderDebugInfo, std::uint32_t shaderDebugInfoSize, void* pUserData)
	{
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: shader debug info received ({} bytes)", shaderDebugInfoSize);
		if (pShaderDebugInfo && shaderDebugInfoSize > 0)
		{
			auto* self = static_cast<AftermathContext*>(pUserData);
			std::filesystem::path baseDir = self->m_crashDumpDir.empty() ? std::filesystem::current_path() / "gpu_crash_dumps" : std::filesystem::path(self->m_crashDumpDir);
			WriteShaderDebugInfoToDisk(baseDir, pShaderDebugInfo, shaderDebugInfoSize);
		}
	}

	void GFSDK_AFTERMATH_CALL AftermathContext::OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addDescription, void* /*pUserData*/)
	{
		if (addDescription)
		{
			addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, "AetherCore");
			addDescription(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, "0.1.0");
		}
	}

	void GFSDK_AFTERMATH_CALL AftermathContext::OnResolveMarker(const void* pMarkerData, std::uint32_t markerDataSize, void* /*pUserData*/, PFN_GFSDK_Aftermath_ResolveMarker resolveMarker)
	{
		if (resolveMarker && pMarkerData)
		{
			resolveMarker(pMarkerData, markerDataSize);
		}
	}

	bool AftermathContext::EnableGpuCrashDumps(const char* crashDumpDir)
	{
		if (m_crashDumpsEnabled)
		{
			return true;
		}

		if (crashDumpDir != nullptr)
		{
			m_crashDumpDir = crashDumpDir;
		}

		GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
		        GFSDK_Aftermath_Version_API,
		        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
		        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
		        OnCrashDump,
		        OnShaderDebugInfo,
		        OnDescription,
		        OnResolveMarker,
		        this);

		if (result != GFSDK_Aftermath_Result_Success)
		{
			AE_WARN(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to enable GPU crash dumps (result={})", static_cast<std::uint32_t>(result));
			return false;
		}

		m_crashDumpsEnabled = true;
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dumps enabled (deferred debug info)");
		return true;
	}

	bool AftermathContext::Initialize(VkDevice device, VkPhysicalDevice /*physicalDevice*/)
	{
		if (m_initialized)
		{
			return true;
		}

		if (!m_crashDumpsEnabled)
		{
			return false;
		}

		// Per-command-buffer tracking and checkpoints are configured via
		// VK_NV_device_diagnostics_config / VK_NV_device_diagnostic_checkpoints
		// at device creation time.  The Aftermath SDK (2025.5.0) does not expose
		// a separate VK_InitializeDevice for Vulkan — the equivalent is handled
		// through the VkDeviceDiagnosticsConfigCreateInfoNV pNext chain.

		m_device = device;
		m_initialized = true;
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: initialized (VkDevice={})", static_cast<void*>(device));
		return true;
	}

	void AftermathContext::SetEventMarker(VkCommandBuffer cmd, std::string_view markerName) const
	{
		if (!m_initialized)
		{
			return;
		}

		vkCmdSetCheckpointNV(cmd, markerName.data());
	}

	void AftermathContext::Shutdown()
	{
		DisableGpuCrashDumps();
		m_device = VK_NULL_HANDLE;
		m_initialized = false;
	}

	void AftermathContext::DisableGpuCrashDumps()
	{
		if (!m_crashDumpsEnabled)
		{
			return;
		}

		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: Disabling GPU crash dumps");
		GFSDK_Aftermath_DisableGpuCrashDumps();
		m_crashDumpsEnabled = false;
	}
} // namespace aether

#endif // AETHER_ENABLE_NVIDIA_AFTERMATH
