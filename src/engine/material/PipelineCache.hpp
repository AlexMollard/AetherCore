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
		std::unordered_multimap<std::uint64_t, Entry> m_entries;
	};
} // namespace aether
