// Included inside BrowserManaged's C linkage block and by native contract tests.
void aether_entity_material_set_color(std::uint32_t id, aether::app::scripting::interop::Vec3 color)
{
	if (!s_world || !s_world->GetRegistry().valid(aether::World::ToEntt(aether::Entity{id})))
	{
		return;
	}
	auto* material = s_world->TryGet<aether::MaterialInstanceComponent>(aether::Entity{id});
	if (!material)
	{
		material = &s_world->Emplace<aether::MaterialInstanceComponent>(aether::Entity{id});
	}
	material->asset.baseColorFactor = glm::vec4(color.x, color.y, color.z, material->asset.baseColorFactor.w);
}

void aether_set_material_texture(std::uint32_t id, const char* path)
{
	if (!s_world || !s_world->GetRegistry().valid(aether::World::ToEntt(aether::Entity{id})))
	{
		return;
	}
	const aether::Entity entity{id};
	if (!s_world->Has<aether::MaterialInstanceComponent>(entity))
	{
		s_world->Emplace<aether::MaterialInstanceComponent>(entity);
	}
	auto* request = s_world->TryGet<aether::app::web::BrowserMaterialTexture>(entity);
	const std::string next = path ? path : "";
	if (request && request->path == next)
	{
		return;
	}
	s_world->EmplaceOrReplace<aether::app::web::BrowserMaterialTexture>(entity, aether::app::web::BrowserMaterialTexture{next, true});
}
