#include "scripting/CSharpGameplayHost.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "scripting/GameplayAbiValidation.hpp"

namespace aether::app::scripting
{
	namespace
	{
		template<typename F>
		AetherGameplayStatus Guard(F&& callback) noexcept
		{
			try
			{
				return callback();
			}
			catch (...)
			{
				return AE_GAMEPLAY_SCRIPT_ERROR;
			}
		}

		CSharpGameplayHost& Self(std::uint64_t context)
		{
			return *reinterpret_cast<CSharpGameplayHost*>(static_cast<std::uintptr_t>(context));
		}

		AetherGameplayUtf8 View(const std::string& text)
		{
			return {text.data(), static_cast<std::uint32_t>(text.size())};
		}
	} // namespace

	Expected<void> CSharpGameplayHost::Reset(const aether::scripting::ManagedScriptApi* api, std::uint32_t generation)
	{
		m_gameplay.Clear(generation);
		m_instances.clear();
		m_types.clear();
		m_api = api;
		if (api == nullptr || api->GetScriptTypeCount == nullptr || api->GetScriptTypeName == nullptr || api->GetPropertyCount == nullptr || api->GetPropertyInfo == nullptr || api->CreateInstance == nullptr || api->DestroyInstance == nullptr
		        || api->InvokeAttach == nullptr || api->InvokeUpdate == nullptr || api->InvokeDetach == nullptr || api->GetProperty == nullptr || api->SetProperty == nullptr || api->GetDefaultProperty == nullptr)
		{
			return std::unexpected(AetherError::Engine("C# gameplay API is unavailable or incomplete"));
		}
		try
		{
			const int count = api->GetScriptTypeCount();
			if (count < 0)
			{
				return std::unexpected(AetherError::Engine("C# script type enumeration failed"));
			}
			std::array<char, 4096> buffer{};
			for (int i = 0; i < count; ++i)
			{
				const int length = api->GetScriptTypeName(i, buffer.data(), static_cast<int>(buffer.size()));
				if (length <= 0 || length >= static_cast<int>(buffer.size()))
				{
					return std::unexpected(AetherError::Engine("C# script type name is invalid or too long"));
				}
				TypeDescriptor descriptor{.name = std::string(buffer.data(), length)};
				const int propertyCount = api->GetPropertyCount(descriptor.name.c_str());
				if (propertyCount < 0)
				{
					return std::unexpected(AetherError::Engine("C# property enumeration failed"));
				}
				for (int j = 0; j < propertyCount; ++j)
				{
					std::int32_t type = 0;
					const int written = api->GetPropertyInfo(descriptor.name.c_str(), j, buffer.data(), static_cast<int>(buffer.size()), &type);
					if (written <= 0 || written >= static_cast<int>(buffer.size()) || type < 0 || type > static_cast<std::int32_t>(AE_GAMEPLAY_PROPERTY_COMPONENT))
					{
						return std::unexpected(AetherError::Engine("C# property metadata is invalid"));
					}
					descriptor.properties.push_back({std::string(buffer.data(), written), static_cast<ScriptPropertyValue::Type>(type)});
				}
				m_types.push_back(std::move(descriptor));
			}
		}
		catch (...)
		{
			m_types.clear();
			return std::unexpected(AetherError::Engine("C# script metadata callback threw"));
		}
		return m_gameplay.Reset(MakeModule(), "csharp", generation);
	}

	AetherGameplayStatus CSharpGameplayHost::Invoke(std::uint64_t handle, void (*callback)(std::uint64_t))
	{
		if (!Has(handle))
		{
			return AE_GAMEPLAY_INVALID_HANDLE;
		}
		const auto before = m_errorSerial.load(std::memory_order_relaxed);
		callback(handle);
		return before == m_errorSerial.load(std::memory_order_relaxed) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
	}

	AetherGameplayStatus CSharpGameplayHost::Read(std::uint64_t handle, std::uint32_t type, std::uint32_t index, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer, bool defaults)
	{
		if (out == nullptr || out->structSize != sizeof(*out) || index > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		if (defaults ? type >= m_types.size() : !Has(handle))
		{
			return AE_GAMEPLAY_INVALID_HANDLE;
		}
		aether::scripting::PropertyValue value{};
		const int success = defaults ? m_api->GetDefaultProperty(m_types[type].name.c_str(), index, &value) : m_api->GetProperty(handle, index, &value);
		if (success == 0)
		{
			return AE_GAMEPLAY_SCRIPT_ERROR;
		}
		const auto tag = static_cast<std::uint32_t>(value.type);
		if (tag > AE_GAMEPLAY_PROPERTY_COMPONENT)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		*out = {.structSize = sizeof(*out), .type = tag};
		if (tag == AE_GAMEPLAY_PROPERTY_FLOAT || tag == AE_GAMEPLAY_PROPERTY_VECTOR3)
		{
			std::copy(std::begin(value.f4), std::end(value.f4), std::begin(out->vector));
		}
		else if (tag == AE_GAMEPLAY_PROPERTY_ENTITY || tag == AE_GAMEPLAY_PROPERTY_COMPONENT)
		{
			out->entity.id = static_cast<std::uint32_t>(value.i64);
		}
		else
		{
			out->integer = value.i64;
		}
		// The legacy string lane also carries enum/component metadata. Preserve it for
		// every tag; the shared ABI uses caller-owned bytes, never the managed scratch pointer.
		const std::size_t length = value.str == nullptr ? 0 : value.reserved >= 0 ? static_cast<std::size_t>(value.reserved) : std::strlen(value.str);
		if (length > std::numeric_limits<std::uint32_t>::max())
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		return aether::scripting::CopyGameplayUtf8({value.str, static_cast<std::uint32_t>(length)}, buffer);
	}

	AetherGameplayModuleApi CSharpGameplayHost::MakeModule()
	{
		AetherGameplayModuleApi module{};
		module.structSize = sizeof(module);
		module.abiMajor = AE_GAMEPLAY_ABI_MAJOR;
		module.context = reinterpret_cast<std::uintptr_t>(this);
		module.capabilities = m_api->InvokeOwnershipChanged != nullptr ? AE_GAMEPLAY_CAP_OWNERSHIP : 0;
		// CoreCLR bootstrap and teardown belong to DotNetHost. No new engine services
		// are installed here: all existing physics-aware C# native exports stay intact.
		module.initialize = [](std::uint64_t, const AetherGameplayEngineApi*) -> AetherGameplayStatus
		{
			return AE_GAMEPLAY_OK;
		};
		module.shutdown = [](std::uint64_t) -> AetherGameplayStatus
		{
			return AE_GAMEPLAY_OK;
		};
		module.getTypeCount = [](std::uint64_t context, std::uint32_t* count) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        if (count == nullptr)
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        *count = static_cast<std::uint32_t>(Self(context).m_types.size());
				        return AE_GAMEPLAY_OK;
			        });
		};
		module.getTypeInfo = [](std::uint64_t context, std::uint32_t index, AetherGameplayTypeInfo* out) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        const auto& types = Self(context).m_types;
				        if (out == nullptr || out->structSize != sizeof(*out) || index >= types.size())
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        const auto& type = types[index];
				        *out = {sizeof(*out), static_cast<std::uint32_t>(type.properties.size()), View(type.name)};
				        return AE_GAMEPLAY_OK;
			        });
		};
		module.getPropertyInfo = [](std::uint64_t context, std::uint32_t type, std::uint32_t index, AetherGameplayPropertyInfo* out) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        const auto& types = Self(context).m_types;
				        if (out == nullptr || out->structSize != sizeof(*out) || type >= types.size() || index >= types[type].properties.size())
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        const auto& property = types[type].properties[index];
				        *out = {sizeof(*out), static_cast<std::uint32_t>(property.type), View(property.name), View(property.enumNames), View(property.componentType)};
				        return AE_GAMEPLAY_OK;
			        });
		};
		module.create = [](std::uint64_t context, std::uint32_t type, AetherGameplayEntity entity, AetherGameplayInstance* out) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        if (out == nullptr || type >= self.m_types.size() || entity.reserved != 0 || entity.world != 0)
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        *out = self.m_api->CreateInstance(self.m_types[type].name.c_str(), entity.id);
				        if (*out == 0)
				        {
					        return AE_GAMEPLAY_SCRIPT_ERROR;
				        }
				        self.m_instances.insert(*out);
				        return AE_GAMEPLAY_OK;
			        });
		};
		module.destroy = [](std::uint64_t context, std::uint64_t handle) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        if (!self.Has(handle))
				        {
					        return AE_GAMEPLAY_INVALID_HANDLE;
				        }
				        self.m_instances.erase(handle);
				        self.m_api->DestroyInstance(handle);
				        return AE_GAMEPLAY_OK;
			        });
		};
		module.attach = [](std::uint64_t context, std::uint64_t handle) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        return self.Invoke(handle, self.m_api->InvokeAttach);
			        });
		};
		module.detach = [](std::uint64_t context, std::uint64_t handle) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        return self.Invoke(handle, self.m_api->InvokeDetach);
			        });
		};
		module.update = [](std::uint64_t context, std::uint64_t handle, float dt) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        if (!self.Has(handle))
				        {
					        return AE_GAMEPLAY_INVALID_HANDLE;
				        }
				        const auto before = self.m_errorSerial.load(std::memory_order_relaxed);
				        self.m_api->InvokeUpdate(handle, dt);
				        return before == self.m_errorSerial.load(std::memory_order_relaxed) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
			        });
		};
		module.getProperty = [](std::uint64_t context, std::uint64_t handle, std::uint32_t index, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer) -> AetherGameplayStatus
		{
			return Guard([&] { return Self(context).Read(handle, 0, index, out, buffer, false); });
		};
		module.getDefaultProperty = [](std::uint64_t context, std::uint32_t type, std::uint32_t index, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer) -> AetherGameplayStatus
		{
			return Guard([&] { return Self(context).Read(0, type, index, out, buffer, true); });
		};
		module.setProperty = [](std::uint64_t context, std::uint64_t handle, std::uint32_t index, const AetherGameplayPropertyValue* input, AetherGameplayUtf8 text) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        if (!self.Has(handle))
				        {
					        return AE_GAMEPLAY_INVALID_HANDLE;
				        }
				        if (input == nullptr || input->structSize != sizeof(*input) || input->type > static_cast<std::int32_t>(AE_GAMEPLAY_PROPERTY_COMPONENT) || index > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
				                || text.length > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) || !aether::scripting::IsGameplayUtf8(text))
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        const std::string owned = text.length == 0 ? std::string{} : std::string(text.data, text.length);
				        aether::scripting::PropertyValue value{};
				        value.type = static_cast<aether::scripting::PropertyType>(input->type);
				        std::copy(std::begin(input->vector), std::end(input->vector), std::begin(value.f4));
				        value.i64 = input->type == AE_GAMEPLAY_PROPERTY_ENTITY || input->type == AE_GAMEPLAY_PROPERTY_COMPONENT ? input->entity.id : input->integer;
				        value.str = owned.c_str();
				        value.reserved = static_cast<std::int32_t>(owned.size());
				        return self.m_api->SetProperty(handle, index, &value) != 0 ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
			        });
		};
		module.dispatchEvent = [](std::uint64_t context, std::uint64_t handle, const AetherGameplayEvent* event) -> AetherGameplayStatus
		{
			return Guard(
			        [&]
			        {
				        auto& self = Self(context);
				        if (!self.Has(handle))
				        {
					        return AE_GAMEPLAY_INVALID_HANDLE;
				        }
				        if (event == nullptr || event->structSize != sizeof(*event))
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        if (event->kind != AE_GAMEPLAY_EVENT_OWNERSHIP || self.m_api->InvokeOwnershipChanged == nullptr)
				        {
					        return AE_GAMEPLAY_UNSUPPORTED;
				        }
				        const auto before = self.m_errorSerial.load(std::memory_order_relaxed);
				        self.m_api->InvokeOwnershipChanged(handle, event->code, (event->flags & 1u) != 0);
				        return before == self.m_errorSerial.load(std::memory_order_relaxed) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
			        });
		};
		return module;
	}
} // namespace aether::app::scripting
