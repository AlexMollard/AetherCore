#include "scripting/DasModuleBase.hpp"
#include "components/GameComponents.hpp"

#include "daScript/daScript.h"

#include "scene/World.hpp"

namespace aether::app::scripting
{
	struct GameComponentsModule : DasModuleBase
	{
		GameComponentsModule()
		      : DasModuleBase("game_components")
		{
			das::ModuleLibrary lib(this);

			// -- Component lifecycle ops (add / has / remove) ----------------
			BIND_COMPONENT("health", aether::HealthComponent)
			BIND_COMPONENT("npc", aether::NpcComponent)
			BIND_COMPONENT("dialogue_state", aether::DialogueStateComponent)

			// -- NpcComponent field accessors --------------------------------
			BIND_STRING_FIELD("npc_id", aether::NpcComponent, npcId)
			BIND_STRING_FIELD("npc_display_name", aether::NpcComponent, displayName)
			BIND_STRING_FIELD("npc_dialogue_id", aether::NpcComponent, dialogueId)
			BIND_FIELD("npc_interaction_radius", aether::NpcComponent, interactionRadius)

			// -- HealthComponent field accessors -----------------------------
			BIND_FIELD("health_current", aether::HealthComponent, current)
			BIND_FIELD("health_max", aether::HealthComponent, max)

			// -- DialogueStateComponent field accessors ----------------------
			BIND_STRING_FIELD("dialogue_state_npc_id", aether::DialogueStateComponent, npcId)
			BIND_STRING_FIELD("dialogue_state_current_node_id", aether::DialogueStateComponent, currentNodeId)

			// -- Iteration helpers -------------------------------------------
			BIND_FOR_EACH("npc", aether::NpcComponent)
			BIND_FOR_EACH("npc_transform", aether::NpcComponent, aether::TransformComponent)
			BIND_FOR_EACH("health", aether::HealthComponent)
			BIND_FOR_EACH("npc_health", aether::NpcComponent, aether::HealthComponent)
			BIND_FOR_EACH("dialogue_state", aether::DialogueStateComponent)

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(GameComponentsModule, aether::app::scripting)