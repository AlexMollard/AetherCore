#include "UiWorld.hpp"

namespace aether::ui
{
	// ── Conversions ───────────────────────────────────────────────────────────
	// We offset by +1 so that the first entt entity (index 0) maps to Entity{1},
	// keeping Entity{0} as the universal "null / invalid" sentinel.

	entt::entity UiWorld::ToEntt(Entity entity) noexcept
	{
		return static_cast<entt::entity>(entity.id - 1u);
	}

	Entity UiWorld::FromEntt(entt::entity entity) noexcept
	{
		return Entity{ static_cast<std::uint32_t>(entt::to_integral(entity)) + 1u };
	}

	// ── Entity lifecycle ──────────────────────────────────────────────────────

	Entity UiWorld::Create()
	{
		return FromEntt(m_registry.create());
	}

	void UiWorld::Destroy(Entity entity)
	{
		if (!entity.IsValid())
		{
			return;
		}
		const entt::entity e = ToEntt(entity);
		if (m_registry.valid(e))
		{
			m_registry.destroy(e);
		}
	}

} // namespace aether::ui
