#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

namespace aether
{
	// Tag component - no data, marks an entity as the player.
	struct PlayerTag
	{
		bool isMainPlayer = true;
	};

	// Hit-points-based health for any living entity.
	struct HealthComponent
	{
		float current = 100.f;
		float max = 100.f;
	};

	// NPC identity and configuration data.
	struct NpcComponent
	{
		std::string npcId;
		std::string displayName;
		std::string dialogueId;
		float interactionRadius = 2.5f;
	};

	// Per-dialogue-session state attached to the active NPC.
	struct DialogueStateComponent
	{
		std::string npcId;
		std::string currentNodeId;
	};
} // namespace aether
