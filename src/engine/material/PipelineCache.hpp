#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "gpu/GpuTypes.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/MaterialTemplate.hpp"

namespace aether
{
	// Content-addressed dedup of GraphicsPipeline objects keyed by MaterialTemplate
	// (spec §4.C). The render-state analogue of ①'s MaterialRegistry. Node-stable
	// (unordered_multimap => stable GraphicsPipeline addresses for PipelineComponent's
	// raw pointers) and NOT ref-counted: the distinct-template working set is tiny and
	// app-lifetime, so we trade a few permanently-resident VkShaderEXT sets for zero
	// lifetime bookkeeping and stable pointers.
	//
	// Thread-safety: Acquire is mutex-guarded. Shutdown must run inside the engine's
	// GPU-idle window (see AssetSubsystem::Shutdown) because GraphicsPipeline::Destroy
	// only schedules deferred destruction.
	class PipelineCache
	{
	public:
		// Frame-graph-constant axes NOT part of the hash key: identical for every
		// scene pipeline in a given frame graph. Supplied once at init.
		// descriptorHeapMappings is a raw pointer owned by BindlessManager that MUST
		// outlive the cache -- shut the cache down before the BindlessManager it borrows.
		struct Context
		{
			gpu::Format colorFormat = gpu::Format::Undefined;
			gpu::Format depthFormat = gpu::Format::Undefined;
			const void* descriptorHeapMappings = nullptr;
		};

		using Factory = std::function<Expected<GraphicsPipeline>(const GraphicsPipeline::Desc&)>;

		void Initialize(Context context, Factory factory);

		// Hash the template, look up; on a hash hit compare template fields before
		// treating it as a match; on miss build via the factory combining tmpl with
		// the Context, store, return a stable pointer. Returns nullptr on build failure.
		[[nodiscard]] const GraphicsPipeline* Acquire(const MaterialTemplate& tmpl);

		// Destroy() every cached pipeline (schedules deferred destruction) and clear.
		// Must run inside the GPU-idle shutdown window.
		void Shutdown();

		[[nodiscard]] std::size_t Size() const;

	private:
		struct Entry
		{
			MaterialTemplate tmpl{};
			GraphicsPipeline pipeline;
		};

		mutable std::mutex m_mutex;
		Context m_context{};
		Factory m_factory;
		std::unordered_multimap<std::uint64_t, Entry> m_entries; // node-stable => &pipeline is durable
	};
} // namespace aether
