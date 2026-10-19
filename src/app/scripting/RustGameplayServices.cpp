#include "RustGameplayServices.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include "IEngineRuntime.hpp"
#include "audio/AudioSubsystem.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "platform/Input.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/UiComponents.hpp"

namespace aether::app::scripting
{
	namespace
	{
		std::recursive_mutex s_services_mutex;
		std::unordered_map<uint64_t, RustGameplayServices*> s_services;
		std::atomic<uint64_t> s_next_token{1};

		std::string Text(AetherGameplayUtf8 text)
		{
			return text.length == 0 ? std::string{} : std::string(text.data, text.length);
		}

		// These names identify entries within engine-owned namespaces, never paths.
		bool SafeName(const std::string& name)
		{
			return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
		}

		std::filesystem::path SaveDirectory()
		{
			const auto root = io::PlatformPaths::GetUserConfigDir();
			if (root.empty())
			{
				return {};
			}
			const auto project = io::PlatformPaths::ReadEnvironmentPath("AETHER_PROJECT_DIR");
			auto source = !project.empty() ? project.lexically_normal() : io::PlatformPaths::GetExecutableDir();
			if (source.filename().empty())
			{
				source = source.parent_path();
			}
			const auto name = source.filename().u8string();
			// Encode the complete UTF-8 project-directory name, preserving uniqueness
			// and keeping Unicode, separators, and platform-reserved names out of paths.
			constexpr char kHex[] = "0123456789abcdef";
			std::string nameSpace = "project-";
			for (unsigned char byte: name)
			{
				nameSpace += kHex[byte >> 4];
				nameSpace += kHex[byte & 15];
			}
			return root / "rust-saves" / nameSpace;
		}
	} // namespace

	RustGameplayServices::RustGameplayServices(SceneContext& context, aether::scripting::GameplayAccess& access)
	      : m_context(context), m_access(access), m_token(s_next_token.fetch_add(1)), m_owner(std::this_thread::get_id()), m_save_directory(SaveDirectory())
	{
		std::lock_guard lock(s_services_mutex);
		s_services.emplace(m_token, this);
	}

	RustGameplayServices::~RustGameplayServices()
	{
		std::lock_guard lock(s_services_mutex);
		s_services.erase(m_token);
	}

	AetherGameplayServicesApi RustGameplayServices::EngineApi() const noexcept
	{
		uint64_t capabilities = 0;
		for (uint32_t operation = 1; operation <= AE_GAMEPLAY_SERVICE_DESTROY_ENTITY; ++operation)
		{
			capabilities |= AE_GAMEPLAY_SERVICE_CAP(operation);
		}
		if (m_context.input == nullptr)
		{
			capabilities &= ~AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_KEY_DOWN);
		}
		if (m_context.physics2D == nullptr)
		{
			capabilities &= ~(AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_GET_VELOCITY_2D) | AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D));
		}
		if (m_context.audio == nullptr)
		{
			capabilities &= ~AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_PLAY_AUDIO);
		}
		if (m_context.services == nullptr || m_context.engineRuntime == nullptr)
		{
			capabilities &= ~AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_INSTANTIATE_PREFAB);
		}
		if (m_save_directory.empty())
		{
			capabilities &= ~(AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_READ_SAVE) | AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_WRITE_SAVE));
		}
		return {sizeof(AetherGameplayServicesApi), AE_GAMEPLAY_SERVICES_ABI_MAJOR, m_token, capabilities, Invoke};
	}

	AetherGameplayStatus RustGameplayServices::Invoke(uint64_t context, const AetherGameplayServiceRequest* request, AetherGameplayPropertyValue* outValue, AetherGameplayUtf8Buffer stringBuffer) noexcept
	{
		try
		{
			std::lock_guard lock(s_services_mutex);
			const auto entry = s_services.find(context);
			if (entry == s_services.end())
			{
				return AE_GAMEPLAY_INVALID_HANDLE;
			}
			auto& services = *entry->second;
			if (services.m_owner != std::this_thread::get_id())
			{
				return AE_GAMEPLAY_WRONG_THREAD;
			}
			if (services.m_invoking)
			{
				return AE_GAMEPLAY_REENTRANT;
			}
			if (request == nullptr || request->structSize != sizeof(*request) || outValue == nullptr || outValue->structSize != sizeof(*outValue))
			{
				return AE_GAMEPLAY_INVALID_ARGUMENT;
			}
			*outValue = {.structSize = sizeof(*outValue)};
			if (!aether::scripting::IsGameplayUtf8(request->text) || !aether::scripting::IsGameplayUtf8(request->text2))
			{
				return AE_GAMEPLAY_INVALID_ARGUMENT;
			}
			for (float lane: request->vector)
			{
				if (!std::isfinite(lane))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
			}
			if (stringBuffer.capacity != 0 && stringBuffer.data == nullptr)
			{
				return AE_GAMEPLAY_INVALID_ARGUMENT;
			}
			services.m_invoking = true;

			struct Reset
			{
				bool& flag;

				~Reset()
				{
					flag = false;
				}
			} reset{services.m_invoking};

			return services.Execute(*request, *outValue, stringBuffer);
		}
		catch (...)
		{
			return AE_GAMEPLAY_SCRIPT_ERROR;
		}
	}

	AetherGameplayStatus RustGameplayServices::Execute(const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer stringBuffer)
	{
		if (m_context.world == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		// Resolve the GameplayAccess token even for operations without an entity. This
		// detects invalidation and mismatched scene worlds before touching the registry.
		const auto access = m_access.EngineApi();
		uint32_t valid = 0;
		const auto probeStatus = access.entityValid(access.context, {}, &valid);
		if (probeStatus != AE_GAMEPLAY_WRONG_WORLD)
		{
			return probeStatus;
		}
		if (!m_access.MatchesWorld(*m_context.world))
		{
			return AE_GAMEPLAY_WRONG_WORLD;
		}
		const bool entityOperation = request.operation == AE_GAMEPLAY_SERVICE_SET_SPRITE_COLOR || request.operation == AE_GAMEPLAY_SERVICE_GET_VELOCITY_2D || request.operation == AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D
		                             || request.operation == AE_GAMEPLAY_SERVICE_SET_UI_TEXT || request.operation == AE_GAMEPLAY_SERVICE_DESTROY_ENTITY;
		if (entityOperation)
		{
			const auto status = access.entityValid(access.context, request.entity, &valid);
			if (status != AE_GAMEPLAY_OK)
			{
				return status;
			}
		}
		auto& world = *m_context.world;
		const Entity entity{request.entity.id};
		const auto text = Text(request.text);
		switch (request.operation)
		{
			case AE_GAMEPLAY_SERVICE_KEY_DOWN:
				if (request.integer < 0 || request.integer > 348)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				if (m_context.input == nullptr)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = m_context.input->IsKeyDown(static_cast<Key>(request.integer)) ? 1 : 0;
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_SET_SPRITE_COLOR:
				if (auto* sprite = world.TryGet<SpriteRendererComponent>(entity))
				{
					sprite->tint = {request.vector[0], request.vector[1], request.vector[2], request.vector[3]};
					return AE_GAMEPLAY_OK;
				}
				return AE_GAMEPLAY_UNSUPPORTED;
			case AE_GAMEPLAY_SERVICE_GET_VELOCITY_2D:
			case AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D:
			{
				if (m_context.physics2D == nullptr)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				const auto* body = world.TryGet<RigidBody2DComponent>(entity);
				if (body == nullptr || !body->body.IsValid())
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				const auto bodyHandle = body->body;
				if (request.operation == AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D)
				{
					m_context.physics2D->SetLinearVelocity(bodyHandle, {request.vector[0], request.vector[1]});
				}
				else
				{
					const auto velocity = m_context.physics2D->GetLinearVelocity(bodyHandle);
					value.type = AE_GAMEPLAY_PROPERTY_VECTOR3;
					value.vector[0] = velocity.x;
					value.vector[1] = velocity.y;
				}
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_FIND_ENTITY:
				for (const auto handle: world.View<NameComponent>())
				{
					if (world.GetRegistry().get<NameComponent>(handle).name == text)
					{
						value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
						value.entity = m_access.Handle(World::FromEntt(handle));
						return AE_GAMEPLAY_OK;
					}
				}
				return AE_GAMEPLAY_INVALID_HANDLE;
			case AE_GAMEPLAY_SERVICE_LOAD_SCENE:
				if (!SafeName(text))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				m_context.pendingSceneLoad = text;
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_INSTANTIATE_PREFAB:
			{
				if (!SafeName(text))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				if (m_context.services == nullptr || m_context.engineRuntime == nullptr)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				const auto prefab = scene::ReadPrefabFile(text);
				if (!prefab)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				Entity created{};
				m_context.engineRuntime->RunExclusive(QuiesceMode::Drain,
				        [&] { created = scene::InstantiatePrefab(*prefab, world, scene::MakeApplySceneDeps(*m_context.services), glm::translate(glm::mat4(1.0f), glm::vec3(request.vector[0], request.vector[1], request.vector[2])), nullptr, true); });
				if (!world.GetRegistry().valid(World::ToEntt(created)))
				{
					return AE_GAMEPLAY_SCRIPT_ERROR;
				}
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = m_access.Handle(created);
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_PLAY_AUDIO:
				if (text.empty() || text.find('\0') != std::string::npos || text.find("..") != std::string::npos || !text.starts_with("project://"))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				if (m_context.audio == nullptr)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = m_context.audio->Play2D(text, {}).id;
				return value.integer != 0 ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
			case AE_GAMEPLAY_SERVICE_SET_UI_TEXT:
				if (auto* uiText = world.TryGet<ui::UIText>(entity))
				{
					uiText->text = text;
					return AE_GAMEPLAY_OK;
				}
				return AE_GAMEPLAY_UNSUPPORTED;
			case AE_GAMEPLAY_SERVICE_READ_SAVE:
			case AE_GAMEPLAY_SERVICE_WRITE_SAVE:
			{
				if (m_save_directory.empty())
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				if (!SafeName(text))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				const auto path = m_save_directory / (text + ".txt");
				if (request.operation == AE_GAMEPLAY_SERVICE_READ_SAVE && !m_context.allowSaveReads)
				{
					return AE_GAMEPLAY_INVALID_HANDLE;
				}
				if (request.operation == AE_GAMEPLAY_SERVICE_WRITE_SAVE)
				{
					if (!io::file_util::CreateDirectories(m_save_directory))
					{
						return AE_GAMEPLAY_SCRIPT_ERROR;
					}
					return io::file_util::WriteText(path, Text(request.text2)) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
				}
				const auto saved = io::file_util::ReadText(path);
				if (!saved)
				{
					return AE_GAMEPLAY_INVALID_HANDLE;
				}
				value.type = AE_GAMEPLAY_PROPERTY_STRING;
				return aether::scripting::CopyGameplayUtf8({saved->data(), static_cast<uint32_t>(saved->size())}, stringBuffer);
			}
			case AE_GAMEPLAY_SERVICE_CREATE_ENTITY:
			{
				const auto created = world.Create();
				world.Emplace<NameComponent>(created, NameComponent{.name = text.empty() ? "Entity" : text});
				world.Emplace<TransformComponent>(created, TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(request.vector[0], request.vector[1], request.vector[2]))});
				world.Emplace<SceneTransientComponent>(created);
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = m_access.Handle(created);
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_DESTROY_ENTITY:
				if (std::find(m_context.pendingDestroys.begin(), m_context.pendingDestroys.end(), entity) == m_context.pendingDestroys.end())
				{
					m_context.pendingDestroys.push_back(entity);
				}
				return AE_GAMEPLAY_OK;
			default:
				return AE_GAMEPLAY_UNSUPPORTED;
		}
	}
} // namespace aether::app::scripting
