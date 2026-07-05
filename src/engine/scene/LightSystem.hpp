#pragma once

#include "scene/System.hpp"

namespace aether
{
	class Renderer;

	// Republishes entity lights (Point/SpotLightComponent + Transform) to the
	// renderer's punctual-light lists every frame: moves, parenting and
	// deletion need no extra bookkeeping. Runs with the registered systems
	// while Playing; the app calls it explicitly (dt = 0) while Editing so
	// light edits and gizmo drags preview live in the frozen scene. Lights
	// are emitted sorted by entity id so per-index shadow bookkeeping stays
	// stable across frames.
	class LightSystem final : public System
	{
	public:
		explicit LightSystem(Renderer& renderer)
		      : m_renderer(renderer)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "LightSystem";
		}

		void Update(World& world, float dt) override;

	private:
		Renderer& m_renderer;
	};
} // namespace aether
