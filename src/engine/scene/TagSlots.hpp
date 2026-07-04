#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace aether
{

	class World;

	constexpr std::size_t kMaxTagSlots = 64;

	template<int N>
	struct TagSlot
	{
	};

	// Tag registry API (implemented in TagSlots.cpp)
	uint32_t TagCreate(const std::string& name);
	uint32_t TagGetId(const std::string& name);
	void TagAdd(World* world, uint32_t entityId, uint32_t tagId);
	bool TagHas(World* world, uint32_t entityId, uint32_t tagId);
	void TagRemove(World* world, uint32_t entityId, uint32_t tagId);
	void ForEachWithTag(World* world, uint32_t tagId, const std::function<void(uint32_t)>& callback);
	// Visits every registered tag (name, id) - the registry is name-keyed, so
	// order is unspecified. Used by tooling (inspector tag section).
	void ForEachTag(const std::function<void(const std::string&, uint32_t)>& callback);

} // namespace aether
