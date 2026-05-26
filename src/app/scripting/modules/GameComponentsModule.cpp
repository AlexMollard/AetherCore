#include "scripting/DasModuleBase.hpp"
#include "components/GameComponents.hpp"

#include "daScript/daScript.h"

#include "scene/World.hpp"

namespace
{
	// ── Helper: deep-copy into a persistent string ──────────────────────────
	// daScript's das::string_view is borrowed memory; we must deep-copy into
	// the component's std::string for lifetime safety.

	static void assign_npc_id(aether::NpcComponent& c, const char* s)
	{
		c.npcId.assign(s ? s : "");
	}

	static void assign_npc_display_name(aether::NpcComponent& c, const char* s)
	{
		c.displayName.assign(s ? s : "");
	}

	static void assign_npc_dialogue_id(aether::NpcComponent& c, const char* s)
	{
		c.dialogueId.assign(s ? s : "");
	}

	static void assign_dlg_npc_id(aether::DialogueStateComponent& c, const char* s)
	{
		c.npcId.assign(s ? s : "");
	}

	static void assign_dlg_current_node_id(aether::DialogueStateComponent& c, const char* s)
	{
		c.currentNodeId.assign(s ? s : "");
	}

	// ── NpcComponent field accessors ────────────────────────────────────────

	char* get_npc_id(aether::World* w, uint32_t id, das::Context* context)
	{
		auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id });

		if (c)
		{
			return context->stringHeap->allocateName(c->npcId);
		}

		// Returning nullptr is perfectly safe; daScript interprets it as an empty string.
		return nullptr;
	}

	void set_npc_id(aether::World* w, uint32_t id, const char* val)
	{
		if (auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id }))
		{
			assign_npc_id(*c, val);
		}
	}

	char* get_npc_display_name(aether::World* w, uint32_t id, das::Context* context)
	{
		auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id });

		if (c)
		{
			return context->stringHeap->allocateName(c->displayName);
		}

		// Returning nullptr is perfectly safe; daScript interprets it as an empty string.
		return nullptr;
	}

	void set_npc_display_name(aether::World* w, uint32_t id, const char* val)
	{
		if (auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id }))
		{
			assign_npc_display_name(*c, val);
		}
	}

	char* get_npc_dialogue_id(aether::World* w, uint32_t id, das::Context* context)
	{
		auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id });

		if (c)
		{
			return context->stringHeap->allocateName(c->dialogueId);
		}

		// Returning nullptr is perfectly safe; daScript interprets it as an empty string.
		return nullptr;
	}

	void set_npc_dialogue_id(aether::World* w, uint32_t id, const char* val)
	{
		if (auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id }))
		{
			assign_npc_dialogue_id(*c, val);
		}
	}

	float get_npc_interaction_radius(aether::World* w, uint32_t id)
	{
		auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id });
		return c ? c->interactionRadius : 0.0f;
	}

	void set_npc_interaction_radius(aether::World* w, uint32_t id, float val)
	{
		if (auto* c = w->TryGet<aether::NpcComponent>(aether::Entity{ id }))
		{
			c->interactionRadius = val;
		}
	}

	// ── HealthComponent field accessors ─────────────────────────────────────

	float get_health_current(aether::World* w, uint32_t id)
	{
		auto* c = w->TryGet<aether::HealthComponent>(aether::Entity{ id });
		return c ? c->current : 0.0f;
	}

	void set_health_current(aether::World* w, uint32_t id, float val)
	{
		if (auto* c = w->TryGet<aether::HealthComponent>(aether::Entity{ id }))
		{
			c->current = val;
		}
	}

	float get_health_max(aether::World* w, uint32_t id)
	{
		auto* c = w->TryGet<aether::HealthComponent>(aether::Entity{ id });
		return c ? c->max : 0.0f;
	}

	void set_health_max(aether::World* w, uint32_t id, float val)
	{
		if (auto* c = w->TryGet<aether::HealthComponent>(aether::Entity{ id }))
		{
			c->max = val;
		}
	}

	// ── DialogueStateComponent field accessors ──────────────────────────────

	char* get_dialogue_state_npc_id(aether::World* w, uint32_t id, das::Context* context)
	{
		auto* c = w->TryGet<aether::DialogueStateComponent>(aether::Entity{ id });

		if (c)
		{
			return context->stringHeap->allocateName(c->npcId);
		}

		// Returning nullptr is perfectly safe; daScript interprets it as an empty string.
		return nullptr;
	}

	void set_dialogue_state_npc_id(aether::World* w, uint32_t id, const char* val)
	{
		if (auto* c = w->TryGet<aether::DialogueStateComponent>(aether::Entity{ id }))
		{
			assign_dlg_npc_id(*c, val);
		}
	}

	char* get_dialogue_state_current_node_id(aether::World* w, uint32_t id, das::Context* context)
	{
		auto* c = w->TryGet<aether::DialogueStateComponent>(aether::Entity{ id });

		if (c)
		{
			return context->stringHeap->allocateName(c->currentNodeId);
		}

		// Returning nullptr is perfectly safe; daScript interprets it as an empty string.
		return nullptr;
	}

	void set_dialogue_state_current_node_id(aether::World* w, uint32_t id, const char* val)
	{
		if (auto* c = w->TryGet<aether::DialogueStateComponent>(aether::Entity{ id }))
		{
			assign_dlg_current_node_id(*c, val);
		}
	}
} // namespace

namespace aether::app::scripting
{
	struct GameComponentsModule : DasModuleBase
	{
		GameComponentsModule()
		      : DasModuleBase("game_components")
		{
			das::ModuleLibrary lib(this);

			// ── Component lifecycle ops (add / has / remove) ────────────────
			BIND_COMPONENT("player_tag", aether::PlayerTag)
			BIND_COMPONENT("health", aether::HealthComponent)
			BIND_COMPONENT("npc", aether::NpcComponent)
			BIND_COMPONENT("dialogue_state", aether::DialogueStateComponent)

			// ── NpcComponent field accessors ────────────────────────────────
			Bind<get_npc_id>(lib, "get_npc_npc_id", SE::accessExternal);
			Bind<set_npc_id>(lib, "set_npc_npc_id", SE::modifyExternal);
			Bind<get_npc_display_name>(lib, "get_npc_display_name", SE::accessExternal);
			Bind<set_npc_display_name>(lib, "set_npc_display_name", SE::modifyExternal);
			Bind<get_npc_dialogue_id>(lib, "get_npc_dialogue_id", SE::accessExternal);
			Bind<set_npc_dialogue_id>(lib, "set_npc_dialogue_id", SE::modifyExternal);
			Bind<get_npc_interaction_radius>(lib, "get_npc_interaction_radius", SE::accessExternal);
			Bind<set_npc_interaction_radius>(lib, "set_npc_interaction_radius", SE::modifyExternal);

			// ── HealthComponent field accessors ─────────────────────────────
			Bind<get_health_current>(lib, "get_health_current", SE::accessExternal);
			Bind<set_health_current>(lib, "set_health_current", SE::modifyExternal);
			Bind<get_health_max>(lib, "get_health_max", SE::accessExternal);
			Bind<set_health_max>(lib, "set_health_max", SE::modifyExternal);

			// ── DialogueStateComponent field accessors ──────────────────────
			Bind<get_dialogue_state_npc_id>(lib, "get_dialogue_state_npc_id", SE::accessExternal);
			Bind<set_dialogue_state_npc_id>(lib, "set_dialogue_state_npc_id", SE::modifyExternal);
			Bind<get_dialogue_state_current_node_id>(lib, "get_dialogue_state_current_node_id", SE::accessExternal);
			Bind<set_dialogue_state_current_node_id>(lib, "set_dialogue_state_current_node_id", SE::modifyExternal);

			// ── Iteration helpers ───────────────────────────────────────────
			BIND_FOR_EACH("player", aether::PlayerTag)
			BIND_FOR_EACH("player_transform", aether::PlayerTag, aether::TransformComponent)
			BIND_FOR_EACH("npc", aether::NpcComponent)
			BIND_FOR_EACH("npc_transform", aether::NpcComponent, aether::TransformComponent)
			BIND_FOR_EACH("health", aether::HealthComponent)
			BIND_FOR_EACH("npc_health", aether::NpcComponent, aether::HealthComponent)

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(GameComponentsModule, aether::app::scripting)
