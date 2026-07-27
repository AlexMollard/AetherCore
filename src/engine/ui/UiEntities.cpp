#include "ui/UiEntities.hpp"

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::ui
{
	namespace
	{
		void EnsureHierarchy(World& world, Entity entity)
		{
			if (!world.Has<HierarchyComponent>(entity))
			{
				world.Emplace<HierarchyComponent>(entity);
			}
		}

		Entity EnsureCanvas(World& world, Entity canvas)
		{
			if (!canvas.IsValid() || !world.GetRegistry().valid(World::ToEntt(canvas)) || !world.Has<UICanvas>(canvas))
			{
				return CreateCanvasEntity(world);
			}
			EnsureHierarchy(world, canvas);
			return canvas;
		}
	} // namespace

	Entity CreateCanvasEntity(World& world)
	{
		const Entity canvas = world.Create();
		world.Emplace<NameComponent>(canvas, NameComponent{.name = "Canvas"});
		world.Emplace<UICanvas>(canvas);
		auto& rect = world.Emplace<UIRect>(canvas);
		rect.anchorMin = {0.f, 0.f};
		rect.anchorMax = {1.f, 1.f};
		rect.offsetMin = {0.f, 0.f};
		rect.offsetMax = {0.f, 0.f};
		world.Emplace<HierarchyComponent>(canvas);
		return canvas;
	}

	Entity CreateImageEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity image = world.Create();
		world.Emplace<NameComponent>(image, NameComponent{.name = "Image"});
		world.Emplace<HierarchyComponent>(image);

		auto& rect = world.Emplace<UIRect>(image);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-100.f, -50.f};
		rect.offsetMax = {100.f, 50.f};

		auto& uiImage = world.Emplace<UIImage>(image);
		uiImage.color = {1.f, 0.18f, 0.14f, 1.f};
		uiImage.cornerRadius = 8.f;

		ecs::SetParent(world, image, canvas);
		return image;
	}

	Entity CreateTextEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity textEntity = world.Create();
		world.Emplace<NameComponent>(textEntity, NameComponent{.name = "Text"});
		world.Emplace<HierarchyComponent>(textEntity);

		auto& rect = world.Emplace<UIRect>(textEntity);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-160.f, -28.f};
		rect.offsetMax = {160.f, 28.f};

		auto& text = world.Emplace<UIText>(textEntity);
		text.text = "Text";
		text.pixelSize = 32.f;

		ecs::SetParent(world, textEntity, canvas);
		return textEntity;
	}

	Entity CreateSliderEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Slider"});
		world.Emplace<HierarchyComponent>(e);

		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-110.f, -12.f};
		rect.offsetMax = {110.f, 12.f};

		world.Emplace<UISlider>(e);
		world.Emplace<UISelectable>(e);

		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateToggleEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Toggle"});
		world.Emplace<HierarchyComponent>(e);

		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-26.f, -13.f};
		rect.offsetMax = {26.f, 13.f};

		world.Emplace<UIToggle>(e);
		world.Emplace<UISelectable>(e);

		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateButtonEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Button"});
		world.Emplace<HierarchyComponent>(e);

		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-120.f, -24.f};
		rect.offsetMax = {120.f, 24.f};

		auto& btn = world.Emplace<UIButton>(e);
		btn.label = "Button";
		world.Emplace<UISelectable>(e);

		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateTextBoxEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "TextBox"});
		world.Emplace<HierarchyComponent>(e);

		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-140.f, -18.f};
		rect.offsetMax = {140.f, 18.f};

		auto& box = world.Emplace<UITextBox>(e);
		box.placeholder = "Enter text";
		world.Emplace<UISelectable>(e);

		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateProgressBarEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "ProgressBar"});
		world.Emplace<HierarchyComponent>(e);

		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-110.f, -8.f};
		rect.offsetMax = {110.f, 8.f};

		world.Emplace<UIProgressBar>(e);

		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateEffectEntity(World& world, Entity canvas, const std::string& shader)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Effect"});
		world.Emplace<HierarchyComponent>(e);

		// Full-screen by default; effects are typically overlays.
		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.f, 0.f};
		rect.anchorMax = {1.f, 1.f};
		rect.offsetMin = {0.f, 0.f};
		rect.offsetMax = {0.f, 0.f};

		auto& fx = world.Emplace<UIEffect>(e);
		fx.shader = shader;

		ecs::SetParent(world, e, canvas);
		return e;
	}
} // namespace aether::ui
