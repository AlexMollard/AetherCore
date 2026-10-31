#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <new>

#include "animation/AnimationDatabase.hpp"
#include "platform/Window.hpp"
#include "vulkan/TransferManager.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "IEngineRuntime.hpp"
#include "rendering/ExclusiveMutation.hpp"
#include "rendering/RenderThreadState.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"

extern "C" void aether_anim_compile(std::uint32_t id);

namespace
{
	struct AnimationRuntime final: aether::IEngineRuntime
	{
		aether::RenderThreadState state;
		unsigned calls = 0;
		void RunExclusive(aether::QuiesceMode, std::function<void()> mutation) override
		{
			++calls;
			aether::RunPausedMutation(state, [] {}, mutation);
		}
		double RealElapsedSeconds() const override { return 0; }
	};

	struct RegistryLifetime
	{
		aether::VulkanContext& context;
		aether::ResourceRegistry registry;
		explicit RegistryLifetime(aether::VulkanContext& ctx): context(ctx)
		{
			registry.Init(ctx.GetDevice().device, ctx.GetAllocator());
			aether::gpu::ResourceRegistry::Initialize({.vulkanDevice = ctx.GetDevice().device, .vmaAllocator = ctx.GetAllocator(), .backendRegistry = &registry});
		}
		~RegistryLifetime()
		{
			aether::GpuHeap::InjectFailureForTesting(aether::GpuHeap::FailurePoint::None);
			vkDeviceWaitIdle(context.GetDevice().device);
			registry.Shutdown();
		}
	};
}

TEST_CASE("Animation replacement retains published state on GPU allocation and upload failure")
{
	const char* enabled = std::getenv("AETHER_TEST_GPU_UPLOADS");
	if (!enabled || std::strcmp(enabled, "1") != 0)
	{
		MESSAGE("GPU animation replacement test requires AETHER_TEST_GPU_UPLOADS=1");
		return;
	}
	using namespace aether;
	Window window{"Animation replacement regression", 320, 240, Window::Mode::Windowed, true};
	auto contextResult = VulkanContext::Create(window, "Animation replacement regression", false, true);
	REQUIRE(contextResult.has_value());
	auto& context = **contextResult;
	RegistryLifetime registry(context);
	assets::GltfAsset asset;
	asset.nodes.resize(1);
	asset.animations.push_back({});
	asset.animations.back().name = "original";
	auto database = AnimationDatabase::Create(context, asset);
	context.GetTransferManager().WaitFor(context.GetTransferManager().LastSubmitted());
	const auto originalAddress = database.GetClipsAddr();
	const AnimationDatabase::GpuClip clip{.nameLength = 3};
	for (const auto failure: {GpuHeap::FailurePoint::DeviceAllocation, GpuHeap::FailurePoint::VirtualBlock,
	        GpuHeap::FailurePoint::Suballocation, GpuHeap::FailurePoint::StagingAllocation, GpuHeap::FailurePoint::FlushAllocation})
	{
		CAPTURE(failure);
		// Staging failure after an earlier submitted copy also exercises partial-upload cleanup.
		GpuHeap::InjectFailureForTesting(failure,
		        failure == GpuHeap::FailurePoint::StagingAllocation || failure == GpuHeap::FailurePoint::FlushAllocation ? 1 : 0);
		const auto result = database.AppendAnimations(std::span(&clip, 1), {}, {}, {}, "new");
		CHECK_FALSE(result.has_value());
		CHECK(database.GetClipCount() == 1);
		CHECK(database.GetClipName(0) == "original");
		CHECK(database.GetClipsAddr() == originalAddress);
	}
	const auto result = database.AppendAnimations(std::span(&clip, 1), {}, {}, {}, "new");
	REQUIRE(result.has_value());
	CHECK(*result == 1);
	CHECK(database.GetClipCount() == 2);
	CHECK(database.GetClipName(1) == "new");
	CHECK(database.GetClipsAddr() != originalAddress);
	context.GetTransferManager().WaitFor(context.GetTransferManager().LastSubmitted());

	// Exercise the actual C ABI used by Animation.Compile, with the production
	// pause helper behind a lightweight runtime (no editor or CoreCLR required).
	World world;
	AnimationRuntime runtime;
	app::scripting::SceneContext scriptContext{.world = &world};
	scriptContext.engineRuntime = &runtime;
	app::scripting::ActiveContextScope active(scriptContext);
	const auto root = world.Create();
	const auto firstChild = world.Create();
	const auto secondChild = world.Create();
	world.Emplace<HierarchyComponent>(root).children = {firstChild, secondChild};
	auto secondDatabase = AnimationDatabase::Create(context, asset);
	world.Emplace<SkinnedMeshComponent>(firstChild).animDb = &database;
	world.Emplace<SkinnedMeshComponent>(secondChild).animDb = &secondDatabase;
	assets::GltfAnimation pending{.name = "pending"};
	world.Get<SkinnedMeshComponent>(firstChild).pendingExternalAnims.push_back(pending);
	world.Get<SkinnedMeshComponent>(secondChild).pendingExternalAnims.push_back(pending);
	GpuHeap::InjectFailureForTesting(GpuHeap::FailurePoint::DeviceAllocation);
	aether_anim_compile(root.id);
	CHECK(runtime.calls == 1);
	CHECK_FALSE(runtime.state.IsReloadInProgress());
	CHECK(database.GetClipCount() == 2);
	CHECK(world.Get<SkinnedMeshComponent>(firstChild).pendingExternalAnims.size() == 1);
	CHECK(secondDatabase.GetClipCount() == 2);
	CHECK(world.Get<SkinnedMeshComponent>(secondChild).pendingExternalAnims.empty());
	aether_anim_compile(root.id);
	CHECK(runtime.calls == 2);
	CHECK_FALSE(runtime.state.IsReloadInProgress());
	CHECK(database.GetClipCount() == 3);
	CHECK(world.Get<SkinnedMeshComponent>(firstChild).pendingExternalAnims.empty());

	// A host allocation exception after the first DMA submission must follow the
	// export's exception barrier, resume admission, and keep both pending lists.
	world.Get<SkinnedMeshComponent>(firstChild).pendingExternalAnims.push_back(pending);
	world.Get<SkinnedMeshComponent>(secondChild).pendingExternalAnims.push_back(pending);
	const auto beforeException = database.GetClipsAddr();
	GpuHeap::InjectFailureForTesting(GpuHeap::FailurePoint::HostBookkeeping, 1);
	aether_anim_compile(root.id);
	CHECK(runtime.calls == 3);
	CHECK_FALSE(runtime.state.IsReloadInProgress());
	CHECK(database.GetClipsAddr() == beforeException);
	CHECK(database.GetClipCount() == 3);
	CHECK(secondDatabase.GetClipCount() == 2);
	CHECK(world.Get<SkinnedMeshComponent>(firstChild).pendingExternalAnims.size() == 1);
	CHECK(world.Get<SkinnedMeshComponent>(secondChild).pendingExternalAnims.size() == 1);
	aether_anim_compile(root.id);
	CHECK(database.GetClipCount() == 4);
	CHECK(secondDatabase.GetClipCount() == 3);
	context.GetTransferManager().WaitFor(context.GetTransferManager().LastSubmitted());
}
