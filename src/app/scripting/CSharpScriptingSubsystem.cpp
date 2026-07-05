#include "scripting/CSharpScriptingSubsystem.hpp"

#include <array>

#include "utils/Logger.hpp"

namespace aether::app::scripting
{
	namespace
	{
		// Locate the deployed managed assemblies (data/scripts/managed) relative to
		// the working directory.
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

	std::vector<ScriptPropertyInfo> CSharpScriptingSubsystem::GetScriptProperties(const std::string& typeName) const
	{
		std::vector<ScriptPropertyInfo> out;
		const auto* api = Api();
		if (api == nullptr || api->GetPropertyCount == nullptr || api->GetPropertyInfo == nullptr)
		{
			return out;
		}
		const int count = api->GetPropertyCount(typeName.c_str());
		std::array<char, 128> buffer{};
		for (int i = 0; i < count; ++i)
		{
			std::int32_t typeTag = 0;
			const int written = api->GetPropertyInfo(typeName.c_str(), i, buffer.data(), static_cast<int>(buffer.size()), &typeTag);
			if (written > 0)
			{
				out.push_back(ScriptPropertyInfo{.name = std::string(buffer.data(), static_cast<size_t>(written)),
					.type = static_cast<aether::ScriptPropertyValue::Type>(typeTag)});
			}
		}
		return out;
	}

	int CSharpScriptingSubsystem::FindPropertyIndex(const std::string& typeName, const std::string& name) const
	{
		const auto props = GetScriptProperties(typeName);
		for (size_t i = 0; i < props.size(); ++i)
		{
			if (props[i].name == name)
			{
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	bool CSharpScriptingSubsystem::GetPropertyValue(std::uint64_t handle, int index, aether::ScriptPropertyValue& out) const
	{
		const auto* api = Api();
		if (api == nullptr || api->GetProperty == nullptr || handle == 0)
		{
			return false;
		}
		aether::scripting::PropertyValue pv{};
		if (api->GetProperty(handle, index, &pv) == 0)
		{
			return false;
		}
		out.type = static_cast<aether::ScriptPropertyValue::Type>(static_cast<std::int32_t>(pv.type));
		out.f4[0] = pv.f4[0];
		out.f4[1] = pv.f4[1];
		out.f4[2] = pv.f4[2];
		out.f4[3] = pv.f4[3];
		out.i64 = pv.i64;
		out.str = pv.str != nullptr ? pv.str : "";
		return true;
	}

	void CSharpScriptingSubsystem::SetPropertyValue(std::uint64_t handle, int index, const aether::ScriptPropertyValue& value) const
	{
		const auto* api = Api();
		if (api == nullptr || api->SetProperty == nullptr || handle == 0)
		{
			return;
		}
		aether::scripting::PropertyValue pv{};
		pv.type = static_cast<aether::scripting::PropertyType>(static_cast<std::int32_t>(value.type));
		pv.f4[0] = value.f4[0];
		pv.f4[1] = value.f4[1];
		pv.f4[2] = value.f4[2];
		pv.f4[3] = value.f4[3];
		pv.i64 = value.i64;
		pv.str = value.str.c_str();
		api->SetProperty(handle, index, &pv);
	}

	bool CSharpScriptingSubsystem::GetDefaultPropertyValue(const std::string& typeName, int index, aether::ScriptPropertyValue& out) const
	{
		const auto* api = Api();
		if (api == nullptr || api->GetDefaultProperty == nullptr)
		{
			return false;
		}
		aether::scripting::PropertyValue pv{};
		if (api->GetDefaultProperty(typeName.c_str(), index, &pv) == 0)
		{
			return false;
		}
		out.type = static_cast<aether::ScriptPropertyValue::Type>(static_cast<std::int32_t>(pv.type));
		out.f4[0] = pv.f4[0];
		out.f4[1] = pv.f4[1];
		out.f4[2] = pv.f4[2];
		out.f4[3] = pv.f4[3];
		out.i64 = pv.i64;
		out.str = pv.str != nullptr ? pv.str : "";
		return true;
	}

	void CSharpScriptingSubsystem::ApplyProperties(
		std::uint64_t handle, const std::string& typeName, const std::map<std::string, aether::ScriptPropertyValue>& props) const
	{
		if (props.empty() || handle == 0)
		{
			return;
		}
		const auto infos = GetScriptProperties(typeName);
		for (const auto& [name, value]: props)
		{
			for (size_t i = 0; i < infos.size(); ++i)
			{
				if (infos[i].name == name)
				{
					SetPropertyValue(handle, static_cast<int>(i), value);
					break;
				}
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
