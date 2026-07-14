#include <string>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <functional>

#include <entt/entt.hpp>

#include "scene/World.hpp"
#include "scene/TagSlots.hpp"

namespace
{
	using namespace aether;

	struct TagOps
	{
		void (*add)(World*, uint32_t entityId);
		bool (*has)(World*, uint32_t entityId);
		void (*remove)(World*, uint32_t entityId);
		void (*for_each)(World*, const std::function<void(uint32_t)>&);
	};

	template<int N>
	struct TagOpsBuilder
	{
		static TagOps build()
		{
			return TagOps{.add = [](World* w, uint32_t id) { w->GetRegistry().emplace_or_replace<TagSlot<N>>(World::ToEntt(Entity{id})); },
			        .has = [](World* w, uint32_t id) -> bool { return w->Has<TagSlot<N>>(Entity{id}); },
			        .remove = [](World* w, uint32_t id) { w->Remove<TagSlot<N>>(Entity{id}); },
			        .for_each =
			                [](World* w, const std::function<void(uint32_t)>& callback)
			        {
				        for (auto enttE: w->View<TagSlot<N>>())
				        {
					        const auto id = static_cast<uint32_t>(entt::to_integral(enttE));
					        callback(id);
				        }
			        }};
		}
	};

	template<int... Is>
	std::array<TagOps, sizeof...(Is)> make_tag_ops_array_impl(std::integer_sequence<int, Is...>)
	{
		return {TagOpsBuilder<Is>::build()...};
	}

	inline std::array<TagOps, kMaxTagSlots> make_tag_ops_array()
	{
		return make_tag_ops_array_impl(std::make_integer_sequence<int, kMaxTagSlots>{});
	}

	class TagRegistry
	{
	public:
		TagRegistry()
		      : m_tagOps(make_tag_ops_array())
		{
		}

		uint32_t CreateTag(const std::string& name)
		{
			auto it = m_nameToId.find(name);
			if (it != m_nameToId.end())
			{
				return it->second;
			}

			if (m_nextTagId >= kMaxTagSlots)
			{
				return UINT32_MAX;
			}

			const uint32_t id = m_nextTagId++;
			m_nameToId[name] = id;
			m_idToName[id] = name;
			return id;
		}

		[[nodiscard]] uint32_t GetTagId(const std::string& name) const
		{
			auto it = m_nameToId.find(name);
			if (it != m_nameToId.end())
			{
				return it->second;
			}
			return UINT32_MAX;
		}

		void AddTag(World* world, uint32_t entityId, uint32_t tagId)
		{
			if (tagId < kMaxTagSlots)
			{
				m_tagOps[tagId].add(world, entityId);
			}
		}

		bool HasTag(World* world, uint32_t entityId, uint32_t tagId) const
		{
			if (tagId < kMaxTagSlots)
			{
				return m_tagOps[tagId].has(world, entityId);
			}
			return false;
		}

		void RemoveTag(World* world, uint32_t entityId, uint32_t tagId)
		{
			if (tagId < kMaxTagSlots)
			{
				m_tagOps[tagId].remove(world, entityId);
			}
		}

		void ForEach(World* world, uint32_t tagId, const std::function<void(uint32_t)>& callback) const
		{
			if (tagId >= kMaxTagSlots)
			{
				return;
			}

			m_tagOps[tagId].for_each(world, callback);
		}

		void ForEachTag(const std::function<void(const std::string&, uint32_t)>& callback) const
		{
			for (const auto& [name, id]: m_nameToId)
			{
				callback(name, id);
			}
		}

	private:
		std::array<TagOps, kMaxTagSlots> m_tagOps;
		std::unordered_map<std::string, uint32_t> m_nameToId;
		std::unordered_map<uint32_t, std::string> m_idToName;
		uint32_t m_nextTagId = 0;
	};

	TagRegistry& GetGlobalTagRegistry()
	{
		static TagRegistry instance;
		return instance;
	}
} // namespace

namespace aether
{
	uint32_t TagCreate(const std::string& name)
	{
		return GetGlobalTagRegistry().CreateTag(name);
	}

	uint32_t TagGetId(const std::string& name)
	{
		return GetGlobalTagRegistry().GetTagId(name);
	}

	void TagAdd(World* world, uint32_t entityId, uint32_t tagId)
	{
		GetGlobalTagRegistry().AddTag(world, entityId, tagId);
	}

	bool TagHas(World* world, uint32_t entityId, uint32_t tagId)
	{
		return GetGlobalTagRegistry().HasTag(world, entityId, tagId);
	}

	void TagRemove(World* world, uint32_t entityId, uint32_t tagId)
	{
		GetGlobalTagRegistry().RemoveTag(world, entityId, tagId);
	}

	void ForEachTag(const std::function<void(const std::string&, uint32_t)>& callback)
	{
		GetGlobalTagRegistry().ForEachTag(callback);
	}

	void ForEachWithTag(World* world, uint32_t tagId, const std::function<void(uint32_t)>& callback)
	{
		GetGlobalTagRegistry().ForEach(world, tagId, callback);
	}

} // namespace aether
