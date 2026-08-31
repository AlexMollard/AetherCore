#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/MaterialTemplate.hpp"

namespace aether
{
	// app-lifetime, so we trade a few permanently-resident VkShaderEXT sets for zero
	class PipelineCache
	{
	public:
		// descriptorHeapMappings is a raw pointer owned by BindlessManager that MUST
		struct Context
		{
			gpu::Format colorFormat = gpu::Format::Undefined;
			gpu::Format depthFormat = gpu::Format::Undefined;
			const void* descriptorHeapMappings = nullptr;
		};

		using Factory = std::function<Expected<GraphicsPipeline>(const GraphicsPipeline::Desc&)>;
		// The same build, split in two so the driver's SPIR-V compile can happen on a worker.
		using PrepareFn = std::function<gpu::ResourceRegistry::PreparedPipeline(const GraphicsPipeline::Desc&)>;
		using CommitFn = std::function<Expected<GraphicsPipeline>(gpu::ResourceRegistry::PreparedPipeline)>;
		using DiscardFn = std::function<void(gpu::ResourceRegistry::PreparedPipeline)>;

		void Initialize(Context context, Factory factory, PrepareFn prepare = {}, CommitFn commit = {}, DiscardFn discard = {});

		[[nodiscard]] const GraphicsPipeline* Acquire(const MaterialTemplate& tmpl);

		// Rebuild every cached pipeline built from `shaderVfsPath`, in place.
		//
		// This exists because a shader can be recompiled while the editor is running, and a
		// cache with no invalidation then keeps handing out the pipeline built from the old
		// bytes - the edit compiles, the material updates, and nothing on screen changes.
		//
		// Rebuilt IN PLACE, in the entry the map already holds: Acquire hands out a pointer
		// into that entry and materials keep it, so replacing the entry rather than its
		// contents would dangle. The pipeline that was there is retired for a few frames
		// first, because the GPU may still be reading it from a frame already submitted.
		//
		// Returns how many pipelines were rebuilt. Safe to call for a path nothing uses.
		std::size_t Reload(std::string_view shaderVfsPath);

		// Reload, split across a thread boundary.
		//
		// A shader object costs the driver tens of milliseconds to build, which on the main
		// thread is several dropped frames every time a material recompiles. PrepareReload
		// does that work and may be called from ANY thread; CommitReload swaps the results in
		// and must run on the thread that owns the resource registry.
		struct PreparedReload
		{
			MaterialTemplate tmpl{};
			gpu::ResourceRegistry::PreparedPipeline prepared{};
		};

		[[nodiscard]] std::vector<PreparedReload> PrepareReload(std::string_view shaderVfsPath);
		std::size_t CommitReload(std::vector<PreparedReload> prepared);
		// Throw away prepared pipelines that will never be committed, so their shader objects
		// are not leaked when the material is switched mid-compile.
		void DiscardPrepared(std::vector<PreparedReload> prepared);

		// Destroy pipelines retired by Reload once no in-flight frame can still be using
		// them. Called once per frame alongside the other deferred-free lists.
		void AdvanceFrame(std::uint64_t frameIndex);

		// Must run inside the GPU-idle shutdown window.
		void Shutdown();

		[[nodiscard]] std::size_t Size() const;

	private:
		// Shared by Acquire and Reload so a rebuilt pipeline is built from exactly the same
		// description as the one it replaces.
		[[nodiscard]] GraphicsPipeline::Desc DescribeFor(const MaterialTemplate& tmpl) const;

		struct Entry
		{
			MaterialTemplate tmpl{};
			GraphicsPipeline pipeline;
		};

		struct Retired
		{
			GraphicsPipeline pipeline;
			std::uint64_t retireFrame = 0;
		};

		mutable std::mutex m_mutex;
		Context m_context{};
		Factory m_factory;
		PrepareFn m_prepare;
		CommitFn m_commit;
		DiscardFn m_discard;
		std::unordered_multimap<std::uint64_t, Entry> m_entries;
		std::vector<Retired> m_retired;
		std::uint64_t m_frameIndex = 0;
	};
} // namespace aether
