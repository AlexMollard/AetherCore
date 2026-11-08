// Included inside BrowserManaged's C linkage block, and by the native contract tests.
// Uses the browser's bound game-thread world; getters preserve hierarchy child order.
void aether_entity_set_parent(std::uint32_t child, std::uint32_t parent)
{
	if (!s_world || !s_world->GetRegistry().valid(aether::World::ToEntt(aether::Entity{child}))
	    || (parent != 0 && !s_world->GetRegistry().valid(aether::World::ToEntt(aether::Entity{parent}))))
	{
		return;
	}
	aether::ecs::SetParent(*s_world, aether::Entity{child}, aether::Entity{parent});
}

std::uint32_t aether_entity_get_parent(std::uint32_t id)
{
	const auto* hierarchy = s_world ? s_world->TryGet<aether::HierarchyComponent>(aether::Entity{id}) : nullptr;
	return hierarchy ? hierarchy->parent.id : 0;
}

std::int32_t aether_entity_child_count(std::uint32_t id)
{
	const auto* hierarchy = s_world ? s_world->TryGet<aether::HierarchyComponent>(aether::Entity{id}) : nullptr;
	return hierarchy ? static_cast<std::int32_t>(hierarchy->children.size()) : 0;
}

std::uint32_t aether_entity_child_at(std::uint32_t id, std::int32_t index)
{
	const auto* hierarchy = s_world ? s_world->TryGet<aether::HierarchyComponent>(aether::Entity{id}) : nullptr;
	if (!hierarchy || index < 0 || static_cast<std::size_t>(index) >= hierarchy->children.size())
	{
		return 0;
	}
	return hierarchy->children[static_cast<std::size_t>(index)].id;
}
