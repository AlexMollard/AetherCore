#include "WebApplication.hpp"
#include "WebMeshFixture.hpp"
#include "WebGameplay.hpp"
#include "platform/web/BrowserKey.hpp"

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
#if defined(AE_WEB_MANAGED)
#include "BrowserManaged.hpp"
#endif
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/GameplayServices.h"
#include "webgpu/WebGpuDevice.hpp"
#include "platform/web/WebAssetLoader.hpp"
#include "platform/web/WebFileSystem.hpp"
#include "io/FileSystem.hpp"
#include "WebAssetFormat.hpp"

namespace
{
	AetherGameplayStatus ReportGameplayDiagnostic(void*, const AetherGameplayDiagnostic& diagnostic)
	{
		EM_ASM(
		        {
			        const decode = (address, length) => new TextDecoder().decode(HEAPU8.subarray(address, address + length));
			        const entry = ({severity : $0, code : decode($1, $2), message : decode($3, $4), file : decode($5, $6), line : $7, column : $8});
			        const logs = aetherRuntime.gameplayLogs ||= [];
			        logs.push(entry);
			        if (logs.length > 256) { logs.shift(); }
			        const method = $0 == 2 ? 'error' : $0 == 1 ? 'warn' : 'log';
			        console[method](entry.code + ': ' + entry.message);
			        if ($0 == 2) { aetherRuntime.diagnostics.push(entry); }
		        },
		        diagnostic.severity, diagnostic.code.data, diagnostic.code.length, diagnostic.message.data, diagnostic.message.length,
		        diagnostic.file.data, diagnostic.file.length, diagnostic.line, diagnostic.column);
		return AE_GAMEPLAY_OK;
	}

	struct BrowserApplication
	{
		aether::World world;
		aether::scripting::GameplayAccess access{world, ReportGameplayDiagnostic};
		AetherGameplayModuleApi module{};
		AetherGameplayInstance instance = 0;
		aether::webgpu::WebGpuDevice graphics;
		aether::web::WebAssetLoader assets, engineAssets;
		std::unique_ptr<aether::app::web::WebGameplay> game;
		bool gameReady = false;
		aether::Entity player;
		std::array<bool, 512> keys{}, pressedKeys{};
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
			game.reset();
			EM_ASM({
				if (globalThis.aetherPlatform)
				{
					aetherPlatform.stop();
				}
			});
			aether::io::FileSystem::Shutdown();
			assets.Stop();
			engineAssets.Stop();
			emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
			emscripten_set_blur_callback("#canvas", nullptr, false, nullptr);
			emscripten_set_visibilitychange_callback(nullptr, false, nullptr);
			{
				aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
				if (attached)
				{
					if (module.detach(module.context, instance) != AE_GAMEPLAY_OK)
					{
						EM_ASM({
							aetherRuntime.status = 'failed';
							aetherRuntime.diagnostics.push({code : 'BROWSER_DETACH', message : 'Gameplay detach failed'});
						});
					}
					attached = false;
				}
				if (instance)
				{
					if (module.destroy(module.context, instance) != AE_GAMEPLAY_OK)
					{
						Failure("Gameplay instance destroy failed");
					}
					instance = 0;
				}
				if (initialized)
				{
					if (module.shutdown(module.context) != AE_GAMEPLAY_OK)
					{
						Failure("Gameplay module shutdown failed");
					}
					initialized = false;
				}
			}
			const auto roots = world.Roots();
			for (const auto root: roots)
			{
				world.Destroy(root);
			}
			access.Invalidate();
#if defined(AE_WEB_MANAGED)
			aether::app::web::BindManagedBrowserWorld(nullptr, nullptr);
#endif
			graphics.Stop();
			EM_ASM({
				aetherRuntime.gameplayStopped = true;
				if (aetherRuntime.status != "failed")
				{
					aetherRuntime.status = "stopped";
				}
			});
		}

		bool Start()
		{
#if defined(AE_WEB_MANAGED)
			aether::app::web::BindManagedBrowserWorld(&world, &access);
#endif
			EM_ASM({ aetherRuntime.assetsReady = false; });
			aether::io::FileSystem::Initialize();
			assets.Start("game.pak");
			engineAssets.Start("engine.pak");
			const bool reference = EM_ASM_INT({ return !!globalThis.aetherProject || new URLSearchParams(location.search).has("reference"); });
			if (!reference)
			{
				player = world.Create();
				world.Emplace<aether::TransformComponent>(player);
				auto& sprite = world.Emplace<aether::SpriteRendererComponent>(player);
				sprite.pixelSize = {2, 2};
				sprite.pixelsPerUnit = 1;
				sprite.pixelArt = true;
				sprite.texturePath = "quadrants";
				const int scenario = EM_ASM_INT({
					const scene = new URLSearchParams(location.search).get('scene');
					return scene == 'layers' ? 1 : scene == 'capacity' ? 2 : 0;
				});
				auto addSprite = [&](float x, glm::vec2 size, glm::vec4 tint, int order)
				{
					const auto entity = world.Create();
					world.Emplace<aether::TransformComponent>(entity).localToWorld[3].x = x;
					auto& sprite = world.Emplace<aether::SpriteRendererComponent>(entity);
					sprite.pixelSize = size;
					sprite.pixelsPerUnit = 1;
					sprite.tint = tint;
					sprite.orderInLayer = order;
					sprite.texturePath = "white";
				};
				if (scenario == 1)
				{
					addSprite(0, {2, 2}, {0, 1, 0, 0.5f}, 2);
					addSprite(0, {1, 1}, {0, 0, 1, 1}, 1);
				}
				if (scenario == 2)
				{
					for (int index = 0; index < 1025; ++index)
					{
						addSprite(index == 1024 ? 0.0f : 10.0f, {2, 2}, {0, 0, 1, 1}, 1 + index);
					}
				}
			}
			if (!Check(aether::scripting::QueryGameplayModule(aether_gameplay_query, 1, sizeof(module), &module), "Gameplay module ABI query failed"))
			{
				return false;
			}
			const auto engine = access.EngineApi();
			if (!Check(module.initialize(module.context, &engine), "Gameplay module initialization failed"))
			{
				return false;
			}
			initialized = true;
			if (reference)
			{
				game = std::make_unique<aether::app::web::WebGameplay>(world, access, graphics, keys, pressedKeys, module);
			}
			AetherGameplayServicesApi services{sizeof(services),
			        1,
			        engine.context,
			        reference ? ((UINT64_C(1) << 17) - 2) : AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_KEY_DOWN), // ops 1-16 (custom pass = 14-16)
			        [](std::uint64_t context, const AetherGameplayServiceRequest* request, AetherGameplayPropertyValue* result, AetherGameplayUtf8Buffer buffer) -> AetherGameplayStatus
			        {
				        auto& app = Get();
				        if (context != app.access.EngineApi().context || !app.initialized)
				        {
					        return AE_GAMEPLAY_INVALID_HANDLE;
				        }
				        if (!request || request->structSize < sizeof(*request) || !result || result->structSize < sizeof(*result))
				        {
					        return AE_GAMEPLAY_INVALID_ARGUMENT;
				        }
				        if (app.game)
				        {
					        return app.game->Invoke(*request, *result, buffer);
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
			if (!Check(aether_gameplay_bind_services(1, sizeof(services), &services), "Gameplay services negotiation failed"))
			{
				return false;
			}
			if (!reference)
			{
				aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
				if (!Check(module.create(module.context, 0, access.Handle(player), &instance), "Gameplay instance creation failed"))
				{
					return false;
				}
				if (!Check(module.attach(module.context, instance), "Gameplay attach failed"))
				{
					return false;
				}
				attached = true;
			}
			auto key = [](int type, const EmscriptenKeyboardEvent* event, void* data) -> bool
			{
				auto& app = *static_cast<BrowserApplication*>(data);
				if (type == EMSCRIPTEN_EVENT_KEYDOWN && !EM_ASM_INT({ return document.activeElement == Module.canvas; }))
				{
					return false;
				}
				const int mapped = aether::web::BrowserKey(event->code);
				if (mapped >= 0)
				{
					app.keys[mapped] = type == EMSCRIPTEN_EVENT_KEYDOWN;
					if (type == EMSCRIPTEN_EVENT_KEYDOWN)
					{
						app.pressedKeys[mapped] = true;
					}
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
				        auto& app = *static_cast<BrowserApplication*>(data);
				        app.keys.fill(false);
				        app.pressedKeys.fill(false);
				        app.previous = 0;
				        app.accumulator = 0;
				        if (app.game)
				        {
					        app.game->ResetTiming();
				        }
				        return false;
			        });
			emscripten_set_blur_callback("#canvas",
			        this,
			        false,
			        [](int, const EmscriptenFocusEvent*, void* data) -> bool
			        {
				        auto& app = *static_cast<BrowserApplication*>(data);
				        app.keys.fill(false);
				        app.pressedKeys.fill(false);
				        return false;
			        });
			emscripten_set_visibilitychange_callback(this,
			        false,
			        [](int, const EmscriptenVisibilityChangeEvent*, void* data) -> bool
			        {
				        auto& app = *static_cast<BrowserApplication*>(data);
				        app.previous = 0;
				        app.accumulator = 0;
				        if (app.game)
				        {
					        app.game->ResetTiming();
				        }
				        app.keys.fill(false);
				        app.pressedKeys.fill(false);
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
			if (assets.GetState() == aether::web::WebAssetLoader::State::Failed || engineAssets.GetState() == aether::web::WebAssetLoader::State::Failed)
			{
				EM_ASM(
				        {
					        aetherRuntime.status = 'failed';
					        aetherRuntime.diagnostics.push({code : 'BROWSER_ASSETS', message : UTF8ToString($0)});
				        },
				        (assets.Error().empty() ? engineAssets.Error() : assets.Error()).c_str());
				return false;
			}
			if (assets.GetState() != aether::web::WebAssetLoader::State::Ready || engineAssets.GetState() != aether::web::WebAssetLoader::State::Ready)
			{
				previous = 0;
				return true;
			}
			aether::web::SetWebPackage(assets.Package(), engineAssets.Package());
			EM_ASM({ aetherRuntime.assetsReady = true; });
			if (graphics.State() == aether::gpu::RenderState::Failed || graphics.State() == aether::gpu::RenderState::DeviceLost)
			{
				return false;
			}
			if (graphics.State() != aether::gpu::RenderState::Ready)
			{
				previous = now;
				return true;
			}
			if (!EM_ASM_INT({ return new URLSearchParams(location.search).has('test'); }))
			{
				double width = 0, height = 0;
				emscripten_get_element_css_size("#canvas", &width, &height);
				graphics.Resize(static_cast<unsigned>(std::max(0.0, std::round(width))), static_cast<unsigned>(std::max(0.0, std::round(height))));
			}
			if (!texturesReady)
			{
				if (!EM_ASM_INT({ return !!globalThis.aetherProject; }))
				{
					const auto texture = aether::io::FileSystem::ReadFile("project://assets/textures/reference.texture");
					WebTextureHeader header;
					if (!texture || !ReadWebTextureHeader(*texture, header))
					{
						Failure("Missing, corrupt or incompatible browser texture");
						return false;
					}
					EM_ASM({ aetherRuntime.assetBytes = $0; }, texture->size());
				}

				if (EM_ASM_INT({ return !!globalThis.aetherPlatform; }))
				{
					for (const auto& path: assets.Package()->Paths())
					{
						if (!path.ends_with(".wav"))
						{
							continue;
						}
						const std::string uri = "project://" + path;
						const auto audio = aether::io::FileSystem::ReadFile(uri);
						if (!audio)
						{
							Failure("Browser audio asset is missing or corrupt");
							return false;
						}
						EM_ASM({ aetherPlatform.prepareAudio(UTF8ToString($0), HEAPU8.slice($1, $1 + $2).buffer); }, uri.c_str(), audio->data(), audio->size());
					}
					EM_ASM({ aetherPlatform.finishAudioPreparation(); });
					const auto font = aether::io::FileSystem::ReadFile("engine://Roboto-Regular.ttf");
					if (!font)
					{
						Failure("Browser Roboto font is missing or corrupt");
						return false;
					}
					EM_ASM({ aetherPlatform.prepareFont(HEAPU8.slice($0, $0 + $1).buffer); }, font->data(), font->size());
				}
				constexpr std::array<std::uint8_t, 16> quadrants{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
				constexpr std::array<std::uint8_t, 4> white{255, 255, 255, 255};
				if (!graphics.UploadTexture(0, 2, 2, quadrants) || !graphics.UploadTexture(1, 1, 1, white))
				{
					Failure("Texture registration failed");
					return false;
				}
				texturesReady = true;
#if AE_WEB_TEST_BRIDGE
				if (aether::app::web::IsMeshFixture() && !aether::app::web::CreateMeshFixture(world, graphics))
				{
					Failure(("Mesh fixture upload failed: " + graphics.MeshUploadError()).c_str());
					return false;
				}
#endif
				if (AE_WEB_TEST_BRIDGE && EM_ASM_INT({ return new URLSearchParams(location.search).has("stopAfterUpload"); }))
				{
					Stop();
					return false;
				}
			}
			if (game)
			{
				if (!gameReady)
				{
					if (EM_ASM_INT({ return aetherPlatform.failed; }))
					{
						Failure("Browser platform initialization failed");
						return false;
					}
					if (!EM_ASM_INT({ return aetherPlatform.storageReady && aetherPlatform.fontReady; }))
					{
						return true;
					}
					const auto initializedGame = game->Initialize();
					if (!initializedGame)
					{
						Failure(initializedGame.error().message.c_str());
						return false;
					}
					gameReady = true;
				}
				const auto frame = game->Frame(now, capture);
				if (!frame)
				{
					Failure(frame.error().message.c_str());
					return false;
				}
				return true;
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
#if AE_WEB_TEST_BRIDGE
			if (aether::app::web::IsMeshFixture())
			{
				int width = 0, height = 0;
				emscripten_get_canvas_element_size("#canvas", &width, &height);
				aether::app::web::ExtractMeshFixture(world, packet, width, height);
			}
#endif
			const bool stopAfterSubmit = (AE_WEB_TEST_BRIDGE && EM_ASM_INT({ return new URLSearchParams(location.search).has("stopAfterSubmit"); }));
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

#if AE_WEB_TEST_BRIDGE
	EMSCRIPTEN_KEEPALIVE void aether_web_lose_device()
	{
		BrowserApplication::Get().graphics.LoseDeviceForTest();
	}
#endif
}

int RunBrowserApplication()
{
	if (AE_WEB_TEST_BRIDGE && EM_ASM_INT({ return new URLSearchParams(location.search).has('dropOwner'); }))
	{
		aether::webgpu::WebGpuDevice pending;
		pending.Initialize();
		return 0;
	}
	auto& application = BrowserApplication::Get();
	if (application.Start())
	{
		if (AE_WEB_TEST_BRIDGE && EM_ASM_INT({ return new URLSearchParams(location.search).has("stopBeforeReady"); }))
		{
			application.Stop();
			return 0;
		}
		emscripten_request_animation_frame_loop(
		        [](double now, void* data) -> bool
		        {
			        auto& app = *static_cast<BrowserApplication*>(data);
			        const bool keepRunning = app.Frame(now);
			        if (!keepRunning && !app.stopped)
			        {
				        app.Stop();
			        }
			        return keepRunning;
		        },
		        &application);
	}
	else
	{
		application.Stop();
	}
	return 0;
}
