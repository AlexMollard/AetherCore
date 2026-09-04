#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>

#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether::app::scripting::interop;

namespace
{
	aether::SpriteRendererComponent* Sprite(std::uint32_t id)
	{
		return ActiveWorld().TryGet<aether::SpriteRendererComponent>(aether::Entity{id});
	}
} // namespace

AE_SCRIPT_API void aether_sprite_set_texture(std::uint32_t id, const char* path)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->texturePath = path != nullptr ? path : "";
	}
	});
}

AE_SCRIPT_API Vec4 aether_sprite_get_tint(std::uint32_t id)
{
	return SafeExport([&] -> Vec4
	{
	if (const auto* sprite = Sprite(id))
	{
		return {sprite->tint.x, sprite->tint.y, sprite->tint.z, sprite->tint.w};
	}
	return {};
	});
}

AE_SCRIPT_API void aether_sprite_set_tint(std::uint32_t id, Vec4 value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->tint = ToGlm(value);
	}
	});
}

AE_SCRIPT_API Vec2 aether_sprite_get_pixel_size(std::uint32_t id)
{
	return SafeExport([&] -> Vec2
	{
	if (const auto* sprite = Sprite(id))
	{
		return {sprite->pixelSize.x, sprite->pixelSize.y};
	}
	return {};
	});
}

AE_SCRIPT_API void aether_sprite_set_pixel_size(std::uint32_t id, Vec2 value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->pixelSize = glm::max(ToGlm(value), glm::vec2(1.0f));
	}
	});
}

AE_SCRIPT_API Vec2 aether_sprite_get_pivot(std::uint32_t id)
{
	return SafeExport([&] -> Vec2
	{
	if (const auto* sprite = Sprite(id))
	{
		return {sprite->pivot.x, sprite->pivot.y};
	}
	return {};
	});
}

AE_SCRIPT_API void aether_sprite_set_pivot(std::uint32_t id, Vec2 value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->pivot = glm::clamp(ToGlm(value), glm::vec2(0.0f), glm::vec2(1.0f));
	}
	});
}

AE_SCRIPT_API float aether_sprite_get_pixels_per_unit(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	const auto* sprite = Sprite(id);
	return sprite != nullptr ? sprite->pixelsPerUnit : 0.0f;
	});
}

AE_SCRIPT_API void aether_sprite_set_pixels_per_unit(std::uint32_t id, float value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->pixelsPerUnit = std::max(value, 0.001f);
	}
	});
}

AE_SCRIPT_API std::int32_t aether_sprite_get_sorting_layer(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* sprite = Sprite(id);
	return sprite != nullptr ? sprite->sortingLayer : 0;
	});
}

AE_SCRIPT_API void aether_sprite_set_sorting_layer(std::uint32_t id, std::int32_t value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->sortingLayer = value;
	}
	});
}

AE_SCRIPT_API std::int32_t aether_sprite_get_order_in_layer(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* sprite = Sprite(id);
	return sprite != nullptr ? sprite->orderInLayer : 0;
	});
}

AE_SCRIPT_API void aether_sprite_set_order_in_layer(std::uint32_t id, std::int32_t value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->orderInLayer = value;
	}
	});
}

AE_SCRIPT_API std::int32_t aether_sprite_get_blend_mode(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* sprite = Sprite(id);
	return sprite != nullptr ? static_cast<std::int32_t>(sprite->blendMode) : 0;
	});
}

AE_SCRIPT_API void aether_sprite_set_blend_mode(std::uint32_t id, std::int32_t value)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->blendMode = static_cast<aether::SpriteBlendMode>(std::clamp(value, 0, 3));
	}
	});
}

AE_SCRIPT_API std::uint32_t aether_sprite_get_flags(std::uint32_t id)
{
	return SafeExport([&] -> std::uint32_t
	{
	const auto* sprite = Sprite(id);
	if (sprite == nullptr)
	{
		return 0;
	}
	return (sprite->visible ? 1u : 0u) | (sprite->flipX ? 2u : 0u) | (sprite->flipY ? 4u : 0u) | (sprite->pixelSnap ? 8u : 0u);
	});
}

AE_SCRIPT_API void aether_sprite_set_flags(std::uint32_t id, std::uint32_t flags)
{
	SafeExport([&] -> void
	{
	if (auto* sprite = Sprite(id))
	{
		sprite->visible = (flags & 1u) != 0u;
		sprite->flipX = (flags & 2u) != 0u;
		sprite->flipY = (flags & 4u) != 0u;
		sprite->pixelSnap = (flags & 8u) != 0u;
	}
	});
}
