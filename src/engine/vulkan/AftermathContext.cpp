#ifdef AETHER_ENABLE_NVIDIA_AFTERMATH

#	include "vulkan/AftermathContext.hpp"

#	include <chrono>
#	include <cstdio>
#	include <ctime>
#	include <filesystem>
#	include <fstream>
#	include <string>
#	include <unordered_map>
#	include <vector>

#	include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		struct ShaderDebugInfoIdentifierHash
		{
			std::size_t operator()(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& id) const
			{
				std::size_t h = 0;
				for (int i = 0; i < 32; ++i)
				{
					h ^= static_cast<std::size_t>(id.id[i]) << ((i % 8) * 8);
				}
				return h;
			}
		};

		struct ShaderDebugInfoIdentifierEqual
		{
			bool operator()(const GFSDK_Aftermath_ShaderDebugInfoIdentifier& a, const GFSDK_Aftermath_ShaderDebugInfoIdentifier& b) const
			{
				for (int i = 0; i < 32; ++i)
				{
					if (a.id[i] != b.id[i])
					{
						return false;
					}
				}
				return true;
			}
		};

		using ShaderDebugInfoMap = std::unordered_map<GFSDK_Aftermath_ShaderDebugInfoIdentifier, std::vector<std::uint8_t>, ShaderDebugInfoIdentifierHash, ShaderDebugInfoIdentifierEqual>;

		ShaderDebugInfoMap s_shaderDebugInfoMap;

		struct ShaderBinaryHashHash
		{
			std::size_t operator()(const GFSDK_Aftermath_ShaderBinaryHash& h) const
			{
				return static_cast<std::size_t>(h.hash);
			}
		};

		struct ShaderBinaryHashEqual
		{
			bool operator()(const GFSDK_Aftermath_ShaderBinaryHash& a, const GFSDK_Aftermath_ShaderBinaryHash& b) const
			{
				return a.hash == b.hash;
			}
		};

		using ShaderBinaryMap = std::unordered_map<GFSDK_Aftermath_ShaderBinaryHash, std::vector<std::uint8_t>, ShaderBinaryHashHash, ShaderBinaryHashEqual>;

		ShaderBinaryMap s_shaderBinaryMap;

		std::filesystem::path BuildDumpPath(const std::filesystem::path& baseDir, const std::string& prefix, const std::string& extension)
		{
			const auto now = std::chrono::system_clock::now();
			const auto timeT = std::chrono::system_clock::to_time_t(now);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

			tm timeInfo;
#	if defined(_MSC_VER)
			localtime_s(&timeInfo, &timeT);
#	else
			localtime_r(&timeT, &timeInfo);
#	endif

			char timeBuf[64];
			std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", &timeInfo);

			char filename[256];
			std::snprintf(filename, sizeof(filename), "%s_%s_%03d%s", prefix.c_str(), timeBuf, static_cast<int>(ms.count()), extension.c_str());

			return baseDir / filename;
		}

		void GFSDK_AFTERMATH_CALL ShaderDebugInfoLookup(const GFSDK_Aftermath_ShaderDebugInfoIdentifier* pIdentifier, PFN_GFSDK_Aftermath_SetData setShaderDebugInfo, void* /*pUserData*/)
		{
			auto it = s_shaderDebugInfoMap.find(*pIdentifier);
			if (it != s_shaderDebugInfoMap.end())
			{
				setShaderDebugInfo(it->second.data(), static_cast<std::uint32_t>(it->second.size()));
			}
		}

		void GFSDK_AFTERMATH_CALL ShaderBinaryLookup(const GFSDK_Aftermath_ShaderBinaryHash* pShaderHash, PFN_GFSDK_Aftermath_SetData setShaderBinary, void* /*pUserData*/)
		{
			auto it = s_shaderBinaryMap.find(*pShaderHash);
			if (it != s_shaderBinaryMap.end())
			{
				setShaderBinary(it->second.data(), static_cast<std::uint32_t>(it->second.size()));
			}
		}

		std::uint32_t ComputeDecoderFlags()
		{
			return GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO & ~GFSDK_Aftermath_GpuCrashDumpDecoderFlags_SHADER_MAPPING_INFO;
		}

		void RegisterShaderBinaryData(const void* pSpirv, uint32_t spirvSize)
		{
			if (!pSpirv || spirvSize == 0)
			{
				return;
			}

			GFSDK_Aftermath_SpirvCode spirvCode{};
			spirvCode.pData = pSpirv;
			spirvCode.size = spirvSize;

			GFSDK_Aftermath_ShaderBinaryHash hash{};
			GFSDK_Aftermath_Result result = GFSDK_Aftermath_GetShaderHashSpirv(GFSDK_Aftermath_Version_API, &spirvCode, &hash);
			if (result != GFSDK_Aftermath_Result_Success)
			{
				return;
			}

			std::vector<std::uint8_t> copy(static_cast<const std::uint8_t*>(pSpirv), static_cast<const std::uint8_t*>(pSpirv) + spirvSize);
			s_shaderBinaryMap[hash] = std::move(copy);
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
				GFSDK_Aftermath_Result decResult = GFSDK_Aftermath_GpuCrashDump_CreateDecoder(GFSDK_Aftermath_Version_API, data, size, &decoder);
				if (decResult != GFSDK_Aftermath_Result_Success)
				{
					AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to create crash dump decoder (result={})", static_cast<std::uint32_t>(decResult));
					return;
				}

				const std::uint32_t decFlags = ComputeDecoderFlags();
				const std::uint32_t jsonFlags = 0u;
				std::uint32_t jsonSize = 0;
				decResult = GFSDK_Aftermath_GpuCrashDump_GenerateJSON(decoder, decFlags, jsonFlags, ShaderDebugInfoLookup, ShaderBinaryLookup, nullptr, nullptr, &jsonSize);
				if (decResult != GFSDK_Aftermath_Result_Success)
				{
					AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: GenerateJSON failed (result={})", static_cast<std::uint32_t>(decResult));
					GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
					return;
				}

				if (jsonSize == 0)
				{
					AE_WARN(LogCategory::Vulkan, "NVIDIA Aftermath: GenerateJSON returned zero size");
					GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
					return;
				}

				std::vector<char> jsonBuf(jsonSize + 1);
				GFSDK_Aftermath_GpuCrashDump_GetJSON(decoder, jsonSize + 1, jsonBuf.data());

				std::streamsize writeSize = static_cast<std::streamsize>(jsonSize);
				if (writeSize > 0 && jsonBuf[writeSize - 1] == '\0')
				{
					writeSize--;
				}

				auto jsonPath = baseDir / (path.stem().string() + ".json");
				std::ofstream jsonOut(jsonPath, std::ios::binary);
				jsonOut.write(jsonBuf.data(), writeSize);
				jsonOut.close();
				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: JSON dump written to {} ({} bytes)", jsonPath.string(), writeSize);

				GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
				s_shaderDebugInfoMap.clear();
				s_shaderBinaryMap.clear();
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

				GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier{};
				GFSDK_Aftermath_Result idResult = GFSDK_Aftermath_GetShaderDebugInfoIdentifier(GFSDK_Aftermath_Version_API, data, size, &identifier);
				if (idResult == GFSDK_Aftermath_Result_Success)
				{
					std::vector<std::uint8_t> copy(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
					s_shaderDebugInfoMap[identifier] = std::move(copy);
				}

				auto path = BuildDumpPath(baseDir, "shader_debug", ".nvdbg");

				std::ofstream out(path, std::ios::binary);
				out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
				out.close();

				AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: shader debug info written to {}", path.string());
			}
			catch (...)
			{
				AE_ERROR(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to write shader debug info to disk");
			}
		}
	} // namespace

	void AftermathContext::RegisterShaderBinary(const void* pSpirv, uint32_t spirvSize)
	{
		RegisterShaderBinaryData(pSpirv, spirvSize);
	}

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
		        GFSDK_Aftermath_Version_API, GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan, GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default, OnCrashDump, OnShaderDebugInfo, OnDescription, OnResolveMarker, this);

		if (result != GFSDK_Aftermath_Result_Success)
		{
			AE_WARN(LogCategory::Vulkan, "NVIDIA Aftermath: Failed to enable GPU crash dumps (result={})", static_cast<std::uint32_t>(result));
			return false;
		}

		m_crashDumpsEnabled = true;
		AE_INFO(LogCategory::Vulkan, "NVIDIA Aftermath: GPU crash dumps enabled");
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
		// a separate VK_InitializeDevice for Vulkan - the equivalent is handled
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
