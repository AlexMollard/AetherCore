#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>

#include "scene/Entity.hpp"
#if !defined(AETHERCORE_WEB_RUNTIME)
// GPU-backed model/material state; the browser runtime has none of it.
#	include "gpu/GpuTypes.hpp"
#	include "scene/LoadedModel.hpp"
#	include "scene/GrassRuntime.hpp"
#	include "material/EffectManager.hpp"
#	include "material/MaterialAsset.hpp"
#endif

namespace aether
{
	class World;
	class AssetManager;
	class CameraManager;
	class Renderer;
	class Input;
	class GraphicsPipeline;
	class PrimitiveMeshes;
	class PhysicsSystem;
	class Physics2DSystem;
	class IEngineRuntime;
	class ServiceContainer;

	namespace audio
	{
		class AudioSubsystem;
	}
	struct DebugVertex;
} // namespace aether

namespace aether::effects
{
	class EffectManager;
}

namespace aether::scripting::gameplay3d
{
	class ClipCatalog;
}

namespace aether::app
{
	class DayNightSystem;
}

namespace aether::app::scripting
{
	struct SceneContext
	{
		aether::World* world = nullptr;
		aether::AssetManager* assets = nullptr;
		aether::CameraManager* cameras = nullptr;
		aether::Renderer* renderer = nullptr;
		aether::Input* input = nullptr;
		aether::ServiceContainer* services = nullptr;
		std::vector<aether::DebugVertex>* debugVertices = nullptr;
		aether::app::DayNightSystem* dayNight = nullptr;
		aether::effects::EffectManager* effects = nullptr;
		aether::PhysicsSystem* physics = nullptr;
		aether::Physics2DSystem* physics2D = nullptr;
		// Clip names/durations for animators without an AnimationDatabase (the browser's cooked glTF
		// clips); null on native, where every skinned mesh has one.
		const aether::scripting::gameplay3d::ClipCatalog* clips = nullptr;
		// Null on a UiShell profile or when audio failed to init; every export
		// treats that as "stay silent".
		aether::audio::AudioSubsystem* audio = nullptr;
		// Used to quiesce the render thread around script-triggered GPU work
		aether::IEngineRuntime* engineRuntime = nullptr;
#if !defined(AETHERCORE_WEB_RUNTIME)
		gpu::CommandPool uploadPool = nullptr;
#endif
		float deltaTime = 0.0f;
		float elapsedTime = 0.0f;
		std::uint64_t frameCount = 0;
		// Isolated agent runs start without persistent saves unless explicitly requested.
		bool allowSaveReads = true;

		std::vector<aether::Entity> sceneEntities;

		// Scene switch requested by a script (Scene.Load). Applied at the end
		// of the script update - a load tears down every entity, so it must
		// never run inside a script callback.
		std::string pendingSceneLoad;
		// Isolated runners validate destinations before the shared loader mutates the world.
		std::function<void(const std::string&)> beforeSceneLoad;

		// Entities destroyed by scripts (Entity.Destroy). Deferred to the end
		// of the script update: an immediate destroy would free the component
		// storage the script runner is iterating.
		std::vector<aether::Entity> pendingDestroys;

#if !defined(AETHERCORE_WEB_RUNTIME)
		// Loaded model data.  Must outlive the mesh entities that reference it.
		std::deque<aether::LoadedModel> loadedModels;
		std::unordered_map<std::string, size_t> loadedModelMap;
		// FileSystem::ContentStamp captured when each entry above was loaded; a mismatch
		// (re-extract / re-bake) triggers a reload while unchanged files reuse the asset.
		std::unordered_map<std::string, std::uint64_t> loadedModelStamps;
		struct GrassFieldCache
		{
			aether::MeshGen::GrassField field;
			std::map<aether::MeshGen::GrassChunkKey, std::shared_ptr<aether::GrassRuntimeChunk>> chunks;
			std::vector<aether::MeshGen::GrassChunkKey> desired;
			bool active = false;
			std::vector<aether::MeshGen::GrassChunkKey> pending;
		};
		struct GrassRootCache
		{
			std::shared_ptr<aether::GrassMaterialState> material;
			aether::MaterialHandle source{};
			float windStrength = 0.0f;
			std::array<glm::vec2, 3> fades{};
		};
		std::unordered_map<std::uint64_t, GrassFieldCache> grassMeshes;
		std::unordered_map<std::uint32_t, GrassRootCache> grassRoots;
		std::vector<glm::vec3> grassCameras;
		std::vector<aether::MeshGen::GrassChunkKey> grassSelectionScratch;

		struct CachedMesh
		{
			const aether::Mesh* mesh = nullptr;
			aether::MaterialAsset materialAsset{};
			std::string displayName;
			std::string kindName;
		};

		aether::PrimitiveMeshes* primitives = nullptr;
		std::vector<CachedMesh> meshCache;
		aether::MaterialAsset defaultMaterial{};
		bool defaultMaterialInitialized = false;
#endif
	};

	extern thread_local SceneContext* g_activeContext;

	// Accessor used inside binding functions. Never returns null during a script call.
	inline SceneContext& ActiveContext()
	{
		return *g_activeContext;
	}

	// Publishes `ctx` as the active context for the duration of a call into managed
	// code, and restores whatever was active before on the way out.
	//
	// EVERY path that invokes a script callback must be wrapped in one of these. The
	// script bodies behind those callbacks call the Net.*/Entity.*/Ui.* exports, and
	// every one of those exports reaches ActiveContext()/ActiveWorld() - which
	// dereferences this pointer unconditionally. Without a scope the callback does not
	// fail gracefully; it segfaults on the first engine call the script makes.
	//
	// Restoring the PREVIOUS value rather than nulling out is what makes it safe to
	// nest: an inbound RPC dispatched from a system tick has no context to restore,
	// while a locally-routed one dispatched from inside a script update must leave the
	// update's own context in place when it returns.
	struct ActiveContextScope
	{
		explicit ActiveContextScope(SceneContext& ctx)
		      : m_previous(g_activeContext)
		{
			g_activeContext = &ctx;
		}

		~ActiveContextScope()
		{
			g_activeContext = m_previous;
		}

		ActiveContextScope(const ActiveContextScope&) = delete;
		ActiveContextScope& operator=(const ActiveContextScope&) = delete;
		ActiveContextScope(ActiveContextScope&&) = delete;
		ActiveContextScope& operator=(ActiveContextScope&&) = delete;

	private:
		SceneContext* m_previous = nullptr;
	};
} // namespace aether::app::scripting
