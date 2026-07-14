#include "material/PipelineCache.hpp"

#include "utils/Logger.hpp"

namespace aether
{
	void PipelineCache::Initialize(Context context, Factory factory)
	{
		const std::scoped_lock lock(m_mutex);
		m_context = context;
		m_factory = std::move(factory);
	}

	const GraphicsPipeline* PipelineCache::Acquire(const MaterialTemplate& tmpl)
	{
		const std::uint64_t hash = HashMaterialTemplate(tmpl);

		const std::scoped_lock lock(m_mutex);
		auto range = m_entries.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it)
		{
			if (it->second.tmpl == tmpl)
			{
				return &it->second.pipeline;
			}
		}

		GraphicsPipeline::Desc desc{};
		desc.shaderVfsPath = tmpl.shaderVfsPath;
		desc.fragmentVfsPath = tmpl.fragmentVfsPath;
		desc.colorFormat = m_context.colorFormat;
		desc.depthFormat = m_context.depthFormat;
		desc.depthTestEnable = true;
		desc.depthWriteEnable = tmpl.depthWriteEnable;
		desc.depthCompareOp = gpu::CompareOp::LessOrEqual;
		desc.blendEnable = tmpl.blendEnable;
		desc.cullMode = tmpl.cullMode;
		desc.descriptorHeapMappings = m_context.descriptorHeapMappings;

		if (!m_factory)
		{
			AE_ERROR(LogCategory::Render, "PipelineCache: no factory configured");
			return nullptr;
		}

		Expected<GraphicsPipeline> built = m_factory(desc);
		if (!built)
		{
			AE_ERROR(LogCategory::Render, "PipelineCache: pipeline build failed for '{}'", tmpl.shaderVfsPath);
			return nullptr;
		}

		auto inserted = m_entries.emplace(hash, Entry{tmpl, std::move(built.value())});
		return &inserted->second.pipeline;
	}

	void PipelineCache::Shutdown()
	{
		const std::scoped_lock lock(m_mutex);
		for (auto& [hash, entry]: m_entries)
		{
			entry.pipeline.Destroy();
		}
		m_entries.clear();
		m_factory = nullptr;
	}

	std::size_t PipelineCache::Size() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_entries.size();
	}
} // namespace aether
