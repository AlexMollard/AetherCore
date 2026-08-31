#include "material/PipelineCache.hpp"

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		// Same rule the slot free lists use: a frame already submitted may still be reading
		// the pipeline, so it cannot be destroyed until those frames have retired.
		constexpr std::uint64_t kPipelineRetireDelayFrames = kMaxFramesInFlight + 1u;
	} // namespace

	void PipelineCache::Initialize(Context context, Factory factory)
	{
		const std::scoped_lock lock(m_mutex);
		m_context = context;
		m_factory = std::move(factory);
	}

	GraphicsPipeline::Desc PipelineCache::DescribeFor(const MaterialTemplate& tmpl) const
	{
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
		return desc;
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

		const GraphicsPipeline::Desc desc = DescribeFor(tmpl);

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

	std::size_t PipelineCache::Reload(const std::string_view shaderVfsPath)
	{
		const std::scoped_lock lock(m_mutex);
		if (!m_factory)
		{
			return 0;
		}

		std::size_t rebuilt = 0;
		for (auto& [hash, entry]: m_entries)
		{
			if (entry.tmpl.shaderVfsPath != shaderVfsPath && entry.tmpl.fragmentVfsPath != shaderVfsPath)
			{
				continue;
			}

			Expected<GraphicsPipeline> built = m_factory(DescribeFor(entry.tmpl));
			if (!built)
			{
				// The old pipeline is left in place on purpose: a shader that now fails to
				// compile should leave the last working version on screen rather than
				// dropping the object out of the frame.
				AE_ERROR(LogCategory::Render, "PipelineCache: reload failed for '{}'; keeping the previous pipeline", entry.tmpl.shaderVfsPath);
				continue;
			}

			m_retired.push_back(Retired{std::move(entry.pipeline), m_frameIndex + kPipelineRetireDelayFrames});
			entry.pipeline = std::move(built.value());
			++rebuilt;
		}
		return rebuilt;
	}

	void PipelineCache::AdvanceFrame(const std::uint64_t frameIndex)
	{
		const std::scoped_lock lock(m_mutex);
		m_frameIndex = frameIndex;
		for (std::size_t i = 0; i < m_retired.size();)
		{
			if (m_retired[i].retireFrame <= m_frameIndex)
			{
				m_retired[i].pipeline.Destroy();
				m_retired[i] = std::move(m_retired.back());
				m_retired.pop_back();
			}
			else
			{
				++i;
			}
		}
	}

	void PipelineCache::Shutdown()
	{
		const std::scoped_lock lock(m_mutex);
		for (auto& [hash, entry]: m_entries)
		{
			entry.pipeline.Destroy();
		}
		m_entries.clear();
		// Anything Reload retired outlives the frame it was retired on, so a shutdown
		// between the two would leak it.
		for (Retired& retired: m_retired)
		{
			retired.pipeline.Destroy();
		}
		m_retired.clear();
		m_factory = nullptr;
	}

	std::size_t PipelineCache::Size() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_entries.size();
	}
} // namespace aether
