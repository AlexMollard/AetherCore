#include "scene/NativeStreamingSnapshot.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "assets/AssetManager.hpp"
#include "io/FileUtil.hpp"
#include "io/FileSystem.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scene/SceneStreamingReferences.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/ServiceContainer.hpp"

#if defined(_WIN32)
#	include <Windows.h>
#endif

namespace aether::app::scene
{
	namespace
	{
		constexpr std::uint64_t kScriptLimit = 1024 * 1024;
		constexpr std::uint64_t kFileLimit = 64ull * 1024 * 1024;
		constexpr std::uint32_t kCountLimit = 100000;
		constexpr std::uint32_t kMagic = 0x53534541;
		constexpr std::uint32_t kVersion = 2;
		std::atomic<std::uint64_t> s_session{0};

		std::filesystem::path RegionPath(const std::filesystem::path& dir, std::string_view region)
		{
			static constexpr char hex[] = "0123456789abcdef";
			std::uint64_t a = 14695981039346656037ull;
			std::uint64_t b = 7809847782465536322ull;
			for (const unsigned char c: region)
			{
				a = (a ^ c) * 1099511628211ull;
				b = (b ^ c) * 14029467366897019727ull;
			}
			std::string name;
			name.reserve(37);
			for (const auto value: {a, b})
			{
				for (int shift = 60; shift >= 0; shift -= 4)
				{
					name.push_back(hex[(value >> shift) & 15]);
				}
			}
			return dir / (name + ".snap");
		}

		bool ValidRegion(std::string_view id)
		{
			return !id.empty() && id.size() <= 255;
		}

		std::uint64_t Hash(const std::byte* bytes, std::size_t size)
		{
			std::uint64_t h = 14695981039346656037ull;
			for (std::size_t i = 0; i < size; ++i)
			{
				h ^= std::to_integer<unsigned char>(bytes[i]);
				h *= 1099511628211ull;
			}
			return h;
		}

		template<class T>
		void Put(std::vector<std::byte>& bytes, T value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* p = reinterpret_cast<const std::byte*>(&value);
			bytes.insert(bytes.end(), p, p + sizeof(value));
		}

		template<class T>
		bool Get(const std::vector<std::byte>& bytes, std::size_t& pos, T& value)
		{
			if (pos > bytes.size() || sizeof(T) > bytes.size() - pos)
			{
				return false;
			}
			std::memcpy(&value, bytes.data() + pos, sizeof(T));
			pos += sizeof(T);
			return true;
		}

		bool Replace(const std::filesystem::path& from, const std::filesystem::path& to)
		{
#if defined(_WIN32)
			return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
			std::error_code ec;
			std::filesystem::rename(from, to, ec);
			return !ec;
#endif
		}

		Unexpected Failure(const char* message)
		{
			return std::unexpected(AetherError::Engine(message));
		}

	} // namespace

	struct NativeStreamingSnapshot::Session
	{
		std::filesystem::path directory;
		std::mutex mutex;
		std::unordered_map<std::string, std::uint64_t> generations;

		~Session()
		{
			if (!directory.empty())
			{
				std::error_code ec;
				std::filesystem::remove_all(directory, ec);
			}
		}
	};

	std::optional<Expected<void>> NativeStreamingSnapshot::SaveTicket::Poll() const
	{
		std::lock_guard lock(mutex);
		return result;
	}

	bool NativeStreamingSnapshot::SaveTicket::Cancel()
	{
		std::lock_guard lock(mutex);
		if (result)
		{
			return false;
		}
		cancelled = true;
		return true;
	}

	std::optional<Expected<std::optional<NativeStreamingSnapshot::Loaded>>> NativeStreamingSnapshot::LoadTicket::Take()
	{
		std::lock_guard lock(mutex);
		auto value = std::move(result);
		result.reset();
		return value;
	}

	NativeStreamingSnapshot::Loaded::~Loaded()
	{
		if (ledger && reserved)
		{
			ledger->Release(streaming::MemoryClass::Staging, reserved);
		}
	}

	NativeStreamingSnapshot::Loaded::Loaded(Loaded&& other) noexcept
	      : scene(std::move(other.scene)),
	        velocities(std::move(other.velocities)),
	        animations(std::move(other.animations)),
	        references(std::move(other.references)),
	        scripts(std::move(other.scripts)),
	        ledger(std::exchange(other.ledger, nullptr)),
	        reserved(std::exchange(other.reserved, 0))
	{
	}

	NativeStreamingSnapshot::Loaded& NativeStreamingSnapshot::Loaded::operator=(Loaded&& other) noexcept
	{
		if (this != &other)
		{
			if (ledger && reserved)
			{
				ledger->Release(streaming::MemoryClass::Staging, reserved);
			}
			scene = std::move(other.scene);
			velocities = std::move(other.velocities);
			animations = std::move(other.animations);
			references = std::move(other.references);
			scripts = std::move(other.scripts);
			ledger = std::exchange(other.ledger, nullptr);
			reserved = std::exchange(other.reserved, 0);
		}
		return *this;
	}

	NativeStreamingSnapshot::NativeStreamingSnapshot(streaming::BudgetLedger& ledger)
	      : m_ledger(ledger)
	{
		Reset();
	}

	NativeStreamingSnapshot::~NativeStreamingSnapshot() = default;

	void NativeStreamingSnapshot::Reset()
	{
		auto session = std::make_shared<Session>();
		std::error_code ec;
		auto root = std::filesystem::temp_directory_path(ec);
		if (!ec)
		{
			session->directory = root / ("aether-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++s_session));
		}
		m_session = std::move(session);
	}

	Expected<std::optional<NativeStreamingSnapshot::SaveOperation>> NativeStreamingSnapshot::CaptureAndSave(scripting::SceneContext& context,
	        ApplySceneDeps& deps,
	        std::string_view region,
	        const std::vector<Entity>& roots,
	        const std::vector<Entity>& entities,
	        const std::vector<Entity>& temporarilyDisabled,
	        const std::vector<Velocity>& velocityOverrides,
	        io::IOPriority priority)
	{
		if (!ValidRegion(region) || !m_session || m_session->directory.empty() || !context.world || !deps.assets || !context.services || entities.size() > kCountLimit)
		{
			return Failure("Invalid streaming snapshot capture context");
		}
		auto& world = *context.world;
		auto* scripts = context.services->TryGet<ScriptComponentSystem>();
		std::uint64_t count = 0;
		for (auto entity: entities)
		{
			if (const auto* component = world.TryGet<ScriptComponent>(entity))
			{
				count += component->scripts.size();
			}
		}
		const auto reservation = std::min(kFileLimit, m_ledger.Limits().stagingBytes);
		const auto workspace = 4ull * 1024 * 1024 + static_cast<std::uint64_t>(entities.size()) * 32768;
		if (count > kCountLimit)
		{
			return Failure("Streaming script count exceeds snapshot limit");
		}
		if (workspace > reservation || count > (reservation - workspace) / kScriptLimit)
		{
			return std::optional<SaveOperation>{};
		}
		if (!reservation || !m_ledger.TryReserve(streaming::MemoryClass::Staging, reservation))
		{
			return std::optional<SaveOperation>{};
		}
		Loaded held;
		held.ledger = &m_ledger;
		held.reserved = reservation;
		std::unordered_map<std::uint32_t, std::uint64_t> entityNodes;
		std::unordered_set<std::uint64_t> nodes;
		for (auto entity: entities)
		{
			auto* node = world.TryGet<SceneNodeComponent>(entity);
			if (!node)
			{
				node = &world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{GenerateSceneNodeId()});
			}
			if (!node->id)
			{
				node->id = GenerateSceneNodeId();
			}
			if (!nodes.insert(node->id).second)
			{
				return Failure("Duplicate streaming node");
			}
			if (!entityNodes.emplace(entity.id, node->id).second)
			{
				return Failure("Duplicate streaming entity");
			}
			if (const auto* rb = world.TryGet<RigidBodyComponent>(entity))
			{
				if (rb->body.IsValid() && !context.physics)
				{
					return Failure("Streaming 3D physics unavailable");
				}
				Velocity v;
				v.node = node->id;
				v.dimensions = 3;
				v.linear = rb->body.IsValid() && context.physics ? context.physics->GetLinearVelocity(rb->body) : rb->initialVelocity;
				v.angular = rb->body.IsValid() && context.physics ? context.physics->GetAngularVelocity(rb->body) : rb->initialAngularVelocity;
				held.velocities.push_back(v);
			}
			if (const auto* rb = world.TryGet<RigidBody2DComponent>(entity))
			{
				Velocity v;
				v.node = node->id;
				v.dimensions = 2;
				if (rb->body.IsValid() && !context.physics2D)
				{
					return Failure("Streaming 2D physics unavailable");
				}
				const auto linear = rb->body.IsValid() && context.physics2D ? context.physics2D->GetLinearVelocity(rb->body) : rb->initialVelocity;
				v.linear = {linear.x, linear.y, 0};
				v.angular.x = rb->body.IsValid() && context.physics2D ? context.physics2D->GetAngularVelocity(rb->body) : rb->initialAngularVelocity;
				held.velocities.push_back(v);
			}
		}
		for (const auto& overrideValue: velocityOverrides)
		{
			const auto it = std::find_if(held.velocities.begin(), held.velocities.end(), [&](const Velocity& value) { return value.node == overrideValue.node && value.dimensions == overrideValue.dimensions; });
			if (it == held.velocities.end())
			{
				return Failure("Invalid pre-freeze streaming velocity");
			}
			*it = overrideValue;
		}
		if (count && !scripts)
		{
			return Failure("Streaming script system unavailable");
		}
		for (auto entity: entities)
		{
			const auto* component = world.TryGet<ScriptComponent>(entity);
			const auto n = component ? component->scripts.size() : 0;
			for (std::size_t i = 0; i < n; ++i)
			{
				auto state = scripts->CaptureStreamingScript(world, entity, static_cast<std::uint32_t>(i));
				if (!state)
				{
					return std::unexpected(std::move(state.error()));
				}
				if (*state)
				{
					if ((**state).state.size() > kScriptLimit)
					{
						return Failure("Streaming script state exceeds limit");
					}
					held.scripts.push_back(std::move(**state));
				}
			}
		}
		held.scene = CaptureSubtrees(world, roots, deps.assets->GetMaterialRegistry(), deps.assets->GetTextureRegistry());
		if (held.scene.entities.size() != entities.size())
		{
			return Failure("Streaming roots do not cover owned entities");
		}
		StreamingNodeIndex referenceIndex(world);
		std::unordered_set<std::uint32_t> temporaryDisabledIds;
		for (const auto entity: temporarilyDisabled)
		{
			if (!entityNodes.contains(entity.id) || !temporaryDisabledIds.insert(entity.id).second)
			{
				return Failure("Invalid temporarily disabled streaming entity");
			}
		}
		for (auto& record: held.scene.entities)
		{
			const auto node = entityNodes.find(record.entityId);
			if (node == entityNodes.end())
			{
				return Failure("Streaming subtree includes non-owned entity");
			}
			if (temporaryDisabledIds.contains(record.entityId))
			{
				record.disabled = false;
			}
			const Entity source{record.entityId};
			if (const auto* sprite = world.TryGet<SpriteRendererComponent>(source); sprite && record.spriteAnimator)
			{
				record.sprite = *sprite;
			}
			if (const auto* animator = world.TryGet<SpriteAnimatorComponent>(source))
			{
				held.animations.push_back(Animation{node->second, animator->frameTime, animator->fixedAccumulator, animator->currentFrame, animator->direction, animator->playing, animator->initialized});
			}
			for (auto& component: record.reflected)
			{
				for (auto& [name, value]: component.fields)
				{
					if (auto result = CaptureStreamingFieldReference(world, source, component.type, name, value, referenceIndex); !result)
					{
						return std::unexpected(result.error());
					}
				}
			}
			record.nodeId = node->second;
			record.sourceNodeId = world.Get<SceneNodeComponent>(source).sourceId;
			record.entityId = 0;
		}
		auto scene = WriteSceneBinary(held.scene);
		std::uint64_t payloadSize = 44 + static_cast<std::uint64_t>(scene.size()) + held.velocities.size() * 33ull + held.animations.size() * 26ull;
		for (const auto& script: held.scripts)
		{
			if (payloadSize > kFileLimit || script.state.size() + 16ull > kFileLimit - payloadSize)
			{
				return Failure("Streaming snapshot exceeds staging limit");
			}
			payloadSize += script.state.size() + 16ull;
		}
		if (payloadSize > reservation / 4 || payloadSize > kFileLimit)
		{
			return Failure("Streaming snapshot exceeds staging limit");
		}
		std::vector<std::byte> bytes;
		bytes.reserve(static_cast<std::size_t>(payloadSize));
		Put(bytes, kMagic);
		Put(bytes, kVersion);
		Put(bytes, std::uint64_t{0});
		Put(bytes, std::uint64_t{0});
		Put(bytes, static_cast<std::uint64_t>(scene.size()));
		Put(bytes, static_cast<std::uint32_t>(held.velocities.size()));
		Put(bytes, static_cast<std::uint32_t>(held.scripts.size()));
		Put(bytes, static_cast<std::uint32_t>(held.animations.size()));
		bytes.insert(bytes.end(), scene.begin(), scene.end());
		for (const auto& v: held.velocities)
		{
			Put(bytes, v.node);
			Put(bytes, v.dimensions);
			for (int i = 0; i < 3; ++i)
			{
				Put(bytes, v.linear[i]);
			}
			for (int i = 0; i < 3; ++i)
			{
				Put(bytes, v.angular[i]);
			}
		}
		for (const auto& a: held.animations)
		{
			Put(bytes, a.node);
			Put(bytes, a.frameTime);
			Put(bytes, a.fixedAccumulator);
			Put(bytes, a.frame);
			Put(bytes, a.direction);
			Put(bytes, static_cast<std::uint8_t>(a.playing));
			Put(bytes, static_cast<std::uint8_t>(a.initialized));
		}
		for (const auto& s: held.scripts)
		{
			Put(bytes, s.node);
			Put(bytes, s.scriptIndex);
			Put(bytes, static_cast<std::uint32_t>(s.state.size()));
			const auto* p = reinterpret_cast<const std::byte*>(s.state.data());
			bytes.insert(bytes.end(), p, p + s.state.size());
		}
		if (bytes.size() > reservation / 4 || bytes.size() > kFileLimit)
		{
			return Failure("Streaming snapshot exceeds staging limit");
		}
		const auto length = static_cast<std::uint64_t>(bytes.size());
		const auto hash = Hash(bytes.data() + 24, bytes.size() - 24);
		std::memcpy(bytes.data() + 8, &length, 8);
		std::memcpy(bytes.data() + 16, &hash, 8);
		const std::unordered_map<std::uint64_t, std::uint64_t> noReferences;
		for (auto& record: held.scene.entities)
		{
			for (auto& component: record.reflected)
			{
				for (auto& [name, value]: component.fields)
				{
					auto saved = value;
					bool found = false;
					if (!StreamingMapField(value, noReferences, true, found))
					{
						return Failure("Invalid streaming reflected reference");
					}
					if (found)
					{
						held.references.push_back(Reference{record.nodeId, component.type, name, std::move(saved)});
					}
				}
			}
		}
		const auto session = m_session;
		const auto path = RegionPath(session->directory, region);
		const std::string regionKey(region);
		std::uint64_t generation;
		{
			std::lock_guard lock(session->mutex);
			generation = ++session->generations[regionKey];
		}
		auto ticket = std::make_shared<SaveTicket>();
		auto payload = std::make_shared<std::vector<std::byte>>(std::move(bytes));
		auto rollback = std::make_shared<Loaded>(std::move(held));
		io::FileSystem::Submit(priority,
		        [session, path, regionKey, generation, ticket, payload, rollback]()
		        {
			        Expected<void> result;
			        auto temp = path;
			        temp += "." + std::to_string(generation) + ".tmp";
			        try
			        {
				        result = io::file_util::WriteBinary(temp, *payload);
				        std::lock_guard ticketLock(ticket->mutex);
				        std::lock_guard sessionLock(session->mutex);
				        if (result && (ticket->cancelled || session->generations[regionKey] != generation))
				        {
					        result = Failure("Streaming snapshot save cancelled or superseded");
				        }
				        if (result && !Replace(temp, path))
				        {
					        result = Failure("Cannot atomically replace streaming snapshot");
				        }
				        if (!result)
				        {
					        std::error_code ec;
					        std::filesystem::remove(temp, ec);
				        }
				        ticket->result = std::move(result);
			        }
			        catch (const std::exception&)
			        {
				        std::error_code ec;
				        std::filesystem::remove(temp, ec);
				        std::lock_guard lock(ticket->mutex);
				        ticket->result = Failure("Streaming snapshot worker failed");
			        }
		        });
		return SaveOperation{std::move(ticket), std::move(rollback)};
	}

	std::shared_ptr<NativeStreamingSnapshot::LoadTicket> NativeStreamingSnapshot::LoadAsync(std::string_view region, io::IOPriority priority)
	{
		auto ticket = std::make_shared<LoadTicket>();
		auto session = m_session;
		auto* ledger = &m_ledger;
		std::string id(region);
		io::FileSystem::Submit(priority,
		        [ticket, session, ledger, id = std::move(id)]()
		        {
			        Expected<std::optional<Loaded>> result;
			        try
			        {
				        result = LoadFromSession(session, *ledger, id);
			        }
			        catch (const std::exception&)
			        {
				        result = Failure("Streaming snapshot load worker failed");
			        }
			        std::lock_guard lock(ticket->mutex);
			        ticket->result = std::move(result);
		        });
		return ticket;
	}

	Expected<std::optional<NativeStreamingSnapshot::Loaded>> NativeStreamingSnapshot::LoadFromSession(const std::shared_ptr<Session>& session, streaming::BudgetLedger& ledger, std::string_view region)
	{
		if (!ValidRegion(region) || !session || session->directory.empty())
		{
			return std::unexpected(AetherError::Engine("Invalid streaming snapshot region"));
		}
		const auto path = RegionPath(session->directory, region);
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec)
		{
			if (ec == std::errc::no_such_file_or_directory)
			{
				return std::optional<Loaded>{};
			}
			return std::unexpected(AetherError::FileSystem("Cannot stat streaming snapshot"));
		}
		if (size < 44 || size > kFileLimit || size > ledger.Limits().stagingBytes / 4)
		{
			return std::unexpected(AetherError::Engine("Invalid streaming snapshot size"));
		}
		const auto reservation = static_cast<std::uint64_t>(size) * 4;
		if (!ledger.TryReserve(streaming::MemoryClass::Staging, reservation))
		{
			return std::unexpected(AetherError::Engine("Streaming snapshot staging budget exhausted"));
		}
		Loaded loaded;
		loaded.ledger = &ledger;
		loaded.reserved = reservation;
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			return std::unexpected(AetherError::FileSystem("Cannot open streaming snapshot"));
		}
		std::vector<std::byte> bytes(static_cast<std::size_t>(size));
		if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) || in.peek() != std::char_traits<char>::eof())
		{
			return std::unexpected(AetherError::FileSystem("Streaming snapshot changed while reading"));
		}
		std::size_t pos = 0;
		std::uint32_t magic, version, velocityCount, scriptCount, animationCount;
		std::uint64_t length, hash, sceneSize;
		if (!Get(bytes, pos, magic) || !Get(bytes, pos, version) || !Get(bytes, pos, length) || !Get(bytes, pos, hash) || !Get(bytes, pos, sceneSize) || !Get(bytes, pos, velocityCount) || !Get(bytes, pos, scriptCount)
		        || !Get(bytes, pos, animationCount) || magic != kMagic || version != kVersion || length != bytes.size() || hash != Hash(bytes.data() + 24, bytes.size() - 24) || velocityCount > kCountLimit * 2 || scriptCount > kCountLimit
		        || animationCount > kCountLimit || sceneSize > bytes.size() - pos || velocityCount > (bytes.size() - pos - sceneSize) / 33 || animationCount > (bytes.size() - pos - sceneSize - velocityCount * 33ull) / 26
		        || scriptCount > (bytes.size() - pos - sceneSize - velocityCount * 33ull - animationCount * 26ull) / 16)
		{
			return std::unexpected(AetherError::Engine("Corrupt streaming snapshot header"));
		}
		auto scene = ReadSceneBinary(bytes.data() + pos, static_cast<std::size_t>(sceneSize));
		if (!scene || scene->entities.size() > kCountLimit)
		{
			return std::unexpected(AetherError::Engine("Invalid streaming snapshot scene"));
		}
		loaded.scene = std::move(*scene);
		pos += static_cast<std::size_t>(sceneSize);
		const std::unordered_map<std::uint64_t, std::uint64_t> none;
		for (auto& record: loaded.scene.entities)
		{
			for (auto& component: record.reflected)
			{
				for (auto& [name, value]: component.fields)
				{
					auto saved = value;
					bool found = false;
					if (!StreamingMapField(value, none, true, found))
					{
						return Failure("Invalid streaming reflected reference");
					}
					if (found)
					{
						loaded.references.push_back(Reference{record.nodeId, component.type, name, std::move(saved)});
					}
				}
			}
		}
		loaded.velocities.reserve(velocityCount);
		for (std::uint32_t i = 0; i < velocityCount; ++i)
		{
			Velocity v;
			if (!Get(bytes, pos, v.node) || !Get(bytes, pos, v.dimensions) || !v.node || (v.dimensions != 2 && v.dimensions != 3))
			{
				return std::unexpected(AetherError::Engine("Invalid streaming velocity"));
			}
			for (int j = 0; j < 3; ++j)
			{
				if (!Get(bytes, pos, v.linear[j]))
				{
					return std::unexpected(AetherError::Engine("Truncated streaming velocity"));
				}
			}
			for (int j = 0; j < 3; ++j)
			{
				if (!Get(bytes, pos, v.angular[j]))
				{
					return std::unexpected(AetherError::Engine("Truncated streaming velocity"));
				}
			}
			loaded.velocities.push_back(v);
		}
		loaded.animations.reserve(animationCount);
		for (std::uint32_t i = 0; i < animationCount; ++i)
		{
			Animation a;
			std::uint8_t playing, initialized;
			if (!Get(bytes, pos, a.node) || !Get(bytes, pos, a.frameTime) || !Get(bytes, pos, a.fixedAccumulator) || !Get(bytes, pos, a.frame) || !Get(bytes, pos, a.direction) || !Get(bytes, pos, playing) || !Get(bytes, pos, initialized) || !a.node
			        || playing > 1 || initialized > 1)
			{
				return Failure("Invalid streaming animation state");
			}
			a.playing = playing != 0;
			a.initialized = initialized != 0;
			loaded.animations.push_back(a);
		}
		loaded.scripts.reserve(scriptCount);
		for (std::uint32_t i = 0; i < scriptCount; ++i)
		{
			ScriptComponentSystem::StreamingScriptState s;
			std::uint32_t stateBytes;
			if (!Get(bytes, pos, s.node) || !Get(bytes, pos, s.scriptIndex) || !Get(bytes, pos, stateBytes) || !s.node || stateBytes > kScriptLimit || stateBytes > bytes.size() - pos)
			{
				return std::unexpected(AetherError::Engine("Invalid streaming script state"));
			}
			s.state.assign(reinterpret_cast<const char*>(bytes.data() + pos), stateBytes);
			pos += stateBytes;
			loaded.scripts.push_back(std::move(s));
		}
		if (pos != bytes.size())
		{
			return std::unexpected(AetherError::Engine("Trailing streaming snapshot data"));
		}
		return std::optional<Loaded>{std::move(loaded)};
	}

	Expected<void> NativeStreamingSnapshot::Restore(scripting::SceneContext& context, const std::vector<Entity>& entities, const Loaded& snapshot)
	{
		if (!context.world || !context.services)
		{
			return Failure("Invalid streaming restore context");
		}
		auto& world = *context.world;
		std::unordered_map<std::uint64_t, Entity> nodes;
		for (auto entity: entities)
		{
			const auto* node = world.TryGet<SceneNodeComponent>(entity);
			if (!node || !node->id || !nodes.emplace(node->id, entity).second)
			{
				return Failure("Invalid restored streaming node");
			}
		}
		StreamingNodeIndex referenceIndex(world);
		for (const auto& reference: snapshot.references)
		{
			const auto entity = nodes.find(reference.node);
			const auto* type = reflect::FindComponentType(reference.component);
			if (entity == nodes.end() || !type || !type->tryGetRaw)
			{
				return Failure("Streaming reflected component missing");
			}
			const auto* field = type->FindField(reference.field);
			auto* component = type->tryGetRaw(world, entity->second);
			if (!field || !field->set || !component)
			{
				return Failure("Streaming reflected field missing");
			}
			if (auto result = ApplyStreamingFieldReference(world, entity->second, *type, *field, component, reference.value, referenceIndex); !result)
			{
				return result;
			}
		}
		for (const auto& animation: snapshot.animations)
		{
			const auto entity = nodes.find(animation.node);
			if (entity == nodes.end())
			{
				return Failure("Streaming animator node missing");
			}
			auto* animator = world.TryGet<SpriteAnimatorComponent>(entity->second);
			if (!animator)
			{
				return Failure("Streaming animator missing");
			}
			animator->frameTime = animation.frameTime;
			animator->fixedAccumulator = animation.fixedAccumulator;
			animator->currentFrame = animation.frame;
			animator->direction = animation.direction;
			animator->playing = animation.playing;
			animator->initialized = animation.initialized;
		}
		for (const auto& v: snapshot.velocities)
		{
			const auto it = nodes.find(v.node);
			if (it == nodes.end())
			{
				return Failure("Streaming velocity node missing");
			}
			if (v.dimensions == 3)
			{
				auto* rb = world.TryGet<RigidBodyComponent>(it->second);
				if (!rb)
				{
					return Failure("Streaming 3D body missing");
				}
				rb->initialVelocity = v.linear;
				rb->initialAngularVelocity = v.angular;
				if (rb->body.IsValid())
				{
					if (!context.physics)
					{
						return Failure("Streaming 3D physics unavailable");
					}
					context.physics->SetLinearVelocity(rb->body, v.linear);
					context.physics->SetAngularVelocity(rb->body, v.angular);
				}
			}
			else
			{
				auto* rb = world.TryGet<RigidBody2DComponent>(it->second);
				if (!rb)
				{
					return Failure("Streaming 2D body missing");
				}
				rb->initialVelocity = {v.linear.x, v.linear.y};
				rb->initialAngularVelocity = v.angular.x;
				if (rb->body.IsValid())
				{
					if (!context.physics2D)
					{
						return Failure("Streaming 2D physics unavailable");
					}
					context.physics2D->SetLinearVelocity(rb->body, rb->initialVelocity);
					context.physics2D->SetAngularVelocity(rb->body, rb->initialAngularVelocity);
				}
			}
		}
		if (!snapshot.scripts.empty())
		{
			auto* scripts = context.services->TryGet<ScriptComponentSystem>();
			if (!scripts)
			{
				return Failure("Streaming script system unavailable");
			}
			return scripts->RestoreStreamingScripts(world, entities, snapshot.scripts);
		}
		return {};
	}

	Expected<void> NativeStreamingSnapshot::PrepareForDetach(World& world, const std::vector<Entity>& entities)
	{
		if (auto* scripts = static_cast<ScriptComponentSystem*>(world.FindSystem("ScriptComponentSystem")))
		{
			if (auto result = scripts->PrepareStreamingScriptReferences(world, entities); !result)
			{
				return result;
			}
		}
		return PrepareStreamingFieldReferencesForDetach(world, entities);
	}

	Expected<void> NativeStreamingSnapshot::ResolveDeferredReferences(scripting::SceneContext& context)
	{
		if (!context.world)
		{
			return Failure("Invalid deferred streaming reference context");
		}
		auto& world = *context.world;
		if (auto* scripts = static_cast<ScriptComponentSystem*>(world.FindSystem("ScriptComponentSystem")))
		{
			if (auto result = scripts->ResolveStreamingScriptReferences(world); !result)
			{
				return result;
			}
		}
		return ResolveStreamingFieldReferences(world);
	}

	void NativeStreamingSnapshot::ForgetDeferredReferences(World& world, const std::vector<Entity>& entities)
	{
		ForgetStreamingFieldReferences(world, entities);
	}
} // namespace aether::app::scene
