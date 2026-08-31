#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gpu/GpuTypes.hpp"
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

		void Initialize(Context context, Factory factory);

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
		std::unordered_multimap<std::uint64_t, Entry> m_entries;
		std::vector<Retired> m_retired;
		std::uint64_t m_frameIndex = 0;
	};
} // namespace aether
