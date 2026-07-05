#include "scripting/CSharpScriptingSubsystem.hpp"

#include <array>

#include "utils/Logger.hpp"

namespace aether::app::scripting
{
	namespace
	{
		// Locate the deployed managed assemblies (data/scripts/managed) relative to
		// the working directory, mirroring ScriptingSubsystem's script search.
		std::filesystem::path ResolveManagedDir()
		{
			const auto cwd = std::filesystem::current_path();
			const std::array<std::filesystem::path, 3> candidates = {
				cwd / "data" / "scripts" / "managed",
				cwd / ".." / "data" / "scripts" / "managed",
				cwd / ".." / ".." / "data" / "scripts" / "managed",
			};
			for (const auto& dir: candidates)
			{
				if (std::filesystem::exists(dir / "AetherCore.Managed.dll"))
				{
					return dir;
				}
			}
			return candidates[0];
		}
	} // namespace

	CSharpScriptingSubsystem::CSharpScriptingSubsystem()
	{
		m_managedDir = ResolveManagedDir();
		m_host.SetErrorHandler([this](const std::string& error) { ReportScriptError(error); });

		if (m_host.Initialize(m_managedDir))
		{
			m_scriptsAssemblyPath = (m_managedDir / "AetherScripts.dll").string();
			LoadScripts();
		}
	}

	const aether::scripting::ManagedScriptApi* CSharpScriptingSubsystem::Api() const
	{
		return m_host.IsAvailable() ? &m_host.Api() : nullptr;
	}

	int CSharpScriptingSubsystem::LoadScripts()
	{
		const auto* api = Api();
		if (api == nullptr || api->LoadScripts == nullptr)
		{
			return -1;
		}

		const int count = api->LoadScripts(m_scriptsAssemblyPath.c_str());
		RefreshTypeNames();
		if (count < 0)
		{
			AE_WARN(LogCategory::App, "C# scripts failed to load from '{}'", m_scriptsAssemblyPath);
		}
		else
		{
			AE_INFO(LogCategory::App, "C# scripts loaded: {} type(s)", count);
		}
		return count;
	}

	void CSharpScriptingSubsystem::RefreshTypeNames()
	{
		m_typeNames.clear();
		const auto* api = Api();
		if (api == nullptr || api->GetScriptTypeCount == nullptr || api->GetScriptTypeName == nullptr)
		{
			return;
		}

		const int count = api->GetScriptTypeCount();
		std::array<char, 256> buffer{};
		for (int i = 0; i < count; ++i)
		{
			const int written = api->GetScriptTypeName(i, buffer.data(), static_cast<int>(buffer.size()));
			if (written > 0)
			{
				m_typeNames.emplace_back(buffer.data(), static_cast<size_t>(written));
			}
		}
	}

	void CSharpScriptingSubsystem::ReportScriptError(const std::string& error)
	{
		AE_ERROR(LogCategory::App, "C# script error: {}", error);
		m_pendingErrors.push_back(error);
	}

	std::vector<std::string> CSharpScriptingSubsystem::PollPendingErrors()
	{
		std::vector<std::string> out;
		out.swap(m_pendingErrors);
		return out;
	}

	void CSharpScriptingSubsystem::ClearErrors()
	{
		m_pendingErrors.clear();
		m_errorsCleared = true;
	}

	bool CSharpScriptingSubsystem::ConsumeErrorsCleared()
	{
		const bool cleared = m_errorsCleared;
		m_errorsCleared = false;
		return cleared;
	}
} // namespace aether::app::scripting
