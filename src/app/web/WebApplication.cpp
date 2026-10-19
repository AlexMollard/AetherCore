#include "WebApplication.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "rendering/RenderFramePacket.hpp"
#include "scene/World.hpp"
#include "scene/Hierarchy.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/GameplayServices.h"
#include "webgpu/WebGpuDevice.hpp"

namespace
{
	struct BrowserApplication
	{
		aether::World world;
		aether::scripting::GameplayAccess access{world};
		AetherGameplayModuleApi module{};
		AetherGameplayInstance instance = 0;
		aether::webgpu::WebGpuDevice graphics;
		aether::Entity player;
		std::array<bool, 512> keys{};
		bool initialized = false, attached = false, stopped = false, texturesReady = false, capture = false;
		unsigned ticks = 0, frames = 0;
		double previous = 0, accumulator = 0;

		void Failure(const char* message)
		{
			EM_ASM(
			        {
				        aetherRuntime.status = 'failed';
				        aetherRuntime.diagnostics.push({code : 'BROWSER_GAMEPLAY', message : UTF8ToString($0)});
			        },
			        message);
			Stop();
		}

		bool Check(AetherGameplayStatus status, const char* operation)
		{
			if (status != AE_GAMEPLAY_OK)
			{
				Failure(operation);
				return false;
			}
			return true;
		}

		void Stop()
		{
			if (stopped)
			{
				return;
			}
			stopped = true;
			emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			{
				aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
				if (attached)
				{
					if (module.detach(module.context, instance) != AE_GAMEPLAY_OK)
					{
						EM_ASM({
							aetherRuntime.status = 'failed';
							aetherRuntime.diagnostics.push({code : 'BROWSER_DETACH', message : 'Rust detach failed'});
						});
					}
					attached = false;
				}
				if (instance)
				{
					(void) module.destroy(module.context, instance);
					instance = 0;
				}
				if (initialized)
				{
					(void) module.shutdown(module.context);
					initialized = false;
				}
			}
			access.Invalidate();
			graphics.Stop();
			EM_ASM({ aetherRuntime.gameplayStopped = true; });
		}

		bool Start()
		{
			player = world.Create();
			world.Emplace<aether::TransformComponent>(player);
			auto& sprite = world.Emplace<aether::SpriteRendererComponent>(player);
			sprite.pixelSize = {2, 2};
			sprite.pixelsPerUnit = 1;
			sprite.pixelArt = true;
			sprite.texturePath = "quadrants";
			if (!Check(aether::scripting::QueryGameplayModule(aether_gameplay_query, 1, sizeof(module), &module), "Rust module ABI query failed"))
			{
				return false;
			}
			const auto engine = access.EngineApi();
			if (!Check(module.initialize(module.context, &engine), "Rust module initialization failed"))
			{
				return false;
			}
			initialized = true;
			AetherGameplayServicesApi services{sizeof(services),
			        1,
			        engine.context,
			        AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_KEY_DOWN),
			        [](std::uint64_t context, const AetherGameplayServiceRequest* request, AetherGameplayPropertyValue* result, AetherGameplayUtf8Buffer) -> AetherGameplayStatus
			        {
				        auto& app = Get();
				        if (context != app.access.EngineApi().context || app.stopped)
				        {
					        return AE_GAMEPLAY_INVALID_HANDLE;
				        }
				        if (!request || request->structSize < sizeof(*request) || !result || result->structSize < sizeof(*result))
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        if (request->operation != AE_GAMEPLAY_SERVICE_KEY_DOWN)
				        {
					        return AE_GAMEPLAY_UNSUPPORTED;
				        }
				        if (request->integer < 0 || request->integer >= 512)
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        *result = {};
				        result->structSize = sizeof(*result);
				        result->type = AE_GAMEPLAY_PROPERTY_BOOL;
				        result->integer = app.keys[request->integer] ? 1 : 0;
				        return AE_GAMEPLAY_OK;
			        }};
			if (!Check(aether_gameplay_bind_services(1, sizeof(services), &services), "Rust services negotiation failed"))
			{
				return false;
			}
			aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
			if (!Check(module.create(module.context, 0, access.Handle(player), &instance), "Rust instance creation failed"))
			{
				return false;
			}
			if (!Check(module.attach(module.context, instance), "Rust attach failed"))
			{
				return false;
			}
			attached = true;
			auto key = [](int type, const EmscriptenKeyboardEvent* event, void* data) -> bool
			{
				auto& app = *static_cast<BrowserApplication*>(data);
				if (std::strcmp(event->code, "ArrowRight") == 0)
				{
					app.keys[262] = type == EMSCRIPTEN_EVENT_KEYDOWN;
					return true;
				}
				if (std::strcmp(event->code, "ArrowLeft") == 0)
				{
					app.keys[263] = type == EMSCRIPTEN_EVENT_KEYDOWN;
					return true;
				}
				return false;
			};
			emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false, key);
			emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false, key);
			emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW,
			        this,
			        false,
			        [](int, const EmscriptenFocusEvent*, void* data) -> bool
			        {
				        static_cast<BrowserApplication*>(data)->keys.fill(false);
				        return false;
			        });
			EM_ASM({ aetherRuntime.status = 'running'; });
			graphics.Resize(64, 64);
			graphics.Initialize();
			return true;
		}

		static BrowserApplication& Get()
		{
			static BrowserApplication application;
			return application;
		}

		bool Frame(double now)
		{
			if (stopped)
			{
				return false;
			}
			if (graphics.State() == aether::gpu::RenderState::Failed || graphics.State() == aether::gpu::RenderState::DeviceLost)
			{
				return false;
			}
			if (graphics.State() != aether::gpu::RenderState::Ready)
			{
				previous = now;
				return true;
			}
			if (!texturesReady)
			{
				constexpr std::array<std::uint8_t, 16> quadrants{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
				constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
				if (!graphics.UploadTexture(0, 2, 2, quadrants) || !graphics.UploadTexture(1, 1, 1, white))
				{
					Failure("Texture registration failed");
					return false;
				}
				texturesReady = true;
			}
			const bool hidden = EM_ASM_INT({ return document.hidden; });
			if (hidden)
			{
				previous = 0;
				accumulator = 0;
				keys.fill(false);
				return true;
			}
			const double delta = previous == 0 ? 0 : std::clamp((now - previous) / 1000.0, 0.0, 0.1);
			previous = now;
			accumulator += delta;
			while (accumulator >= 1.0 / 60.0)
			{
				aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
				if (!Check(module.update(module.context, instance, 1.0f / 60.0f), "Rust fixed update failed"))
				{
					return false;
				}
				++ticks;
				accumulator -= 1.0 / 60.0;
			}
			aether::RenderFramePacket packet;
			packet.frameIndex = frames;
			packet.hasCameraData = true;
			packet.proj[1][1] = -1;
			auto view = world.GetRegistry().view<const aether::TransformComponent, const aether::SpriteRendererComponent>(entt::exclude<aether::DisabledComponent>);
			for (auto raw: view)
			{
				const auto entity = aether::World::FromEntt(raw);
				const auto& sprite = view.get<const aether::SpriteRendererComponent>(raw);
				if (!sprite.visible || aether::ecs::HasDisabledAncestor(world, entity))
				{
					continue;
				}
				if (!sprite.atlasPath.empty())
				{
					Failure("Sprite atlas assets require browser asset support");
					return false;
				}
				const auto bias = [](int value)
				{
					return std::uint64_t(std::clamp(value, -32768, 32767) + 32768);
				};
				const auto flags = (sprite.flipX ? 1u : 0u) | (sprite.flipY ? 2u : 0u) | (sprite.pixelSnap ? 4u : 0u) | (sprite.pixelArt ? 16u : 0u);
				packet.render2D.sprites.push_back({.world = view.get<const aether::TransformComponent>(raw).localToWorld,
				        .uvRect = sprite.uvRect,
				        .color = sprite.tint,
				        .sizeAndPivot = {sprite.pixelSize.x / std::max(sprite.pixelsPerUnit, 0.001f), sprite.pixelSize.y / std::max(sprite.pixelsPerUnit, 0.001f), sprite.pivot.x, sprite.pivot.y},
				        .sortKey = (bias(sprite.sortingLayer) << 48u) | (bias(sprite.orderInLayer) << 32u) | entity.id,
				        .textureIndex = sprite.texturePath == "quadrants" ? 0u : 1u,
				        .entityId = entity.id,
				        .flags = static_cast<aether::SpriteInstanceFlags>(flags),
				        .blendMode = static_cast<unsigned>(sprite.blendMode)});
			}
			const bool stopAfterSubmit = EM_ASM_INT({ return new URLSearchParams(location.search).has("stopAfterSubmit"); });
			if (graphics.Draw(packet, capture || stopAfterSubmit))
			{
				++frames;
				capture = false;
				if (stopAfterSubmit)
				{
					Stop();
					return false;
				}
			}
			const float x = world.TryGet<aether::TransformComponent>(player)->localToWorld[3].x;
			EM_ASM(
			        {
				        aetherRuntime.ticks = $0;
				        aetherRuntime.frames = $1;
				        aetherRuntime.x = $2;
				        aetherRuntime.rendererReady = true;
			        },
			        ticks,
			        frames,
			        x);
			return true;
		}
	};
} // namespace

extern "C"
{
	EMSCRIPTEN_KEEPALIVE void aether_web_capture()
	{
		BrowserApplication::Get().capture = true;
	}

	EMSCRIPTEN_KEEPALIVE void aether_web_resize(unsigned width, unsigned height)
	{
		BrowserApplication::Get().graphics.Resize(width, height);
	}

	EMSCRIPTEN_KEEPALIVE void aether_web_stop()
	{
		BrowserApplication::Get().Stop();
	}

	EMSCRIPTEN_KEEPALIVE void aether_web_lose_device()
	{
		BrowserApplication::Get().graphics.LoseDeviceForTest();
	}
}

int RunBrowserApplication()
{
	auto& application = BrowserApplication::Get();
	if (application.Start())
	{
		if (EM_ASM_INT({ return new URLSearchParams(location.search).has("stopBeforeReady"); }))
		{
			application.Stop();
			return 0;
		}
		emscripten_request_animation_frame_loop([](double now, void* data) -> bool { return static_cast<BrowserApplication*>(data)->Frame(now); }, &application);
	}
	return 0;
}
