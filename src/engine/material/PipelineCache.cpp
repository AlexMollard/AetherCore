#include "material/PipelineCache.hpp"

#include <utility>

#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		// Same rule the slot free lists use: a frame already submitted may still be reading
		// the pipeline, so it cannot be destroyed until those frames have retired.
		constexpr std::uint64_t kPipelineRetireDelayFrames = kMaxFramesInFlight + 1u;
	} // namespace

	void PipelineCache::Initialize(Context context, Factory factory, PrepareFn prepare, CommitFn commit, DiscardFn discard)
	{
		const std::scoped_lock lock(m_mutex);
		m_context = context;
		m_factory = std::move(factory);
		m_prepare = std::move(prepare);
		m_commit = std::move(commit);
		m_discard = std::move(discard);
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

	std::vector<PipelineCache::PreparedReload> PipelineCache::PrepareReload(const std::string_view shaderVfsPath)
	{
		std::vector<MaterialTemplate> matching;
		PrepareFn prepare;
		std::vector<GraphicsPipeline::Desc> descs;
		{
			// The lock is held only to SNAPSHOT what needs rebuilding. Building the pipelines
			// under it would block Acquire on the main thread for the whole duration, which is
			// the stall this exists to remove.
			const std::scoped_lock lock(m_mutex);
			if (!m_prepare)
			{
				return {};
			}
			prepare = m_prepare;
			for (const auto& [hash, entry]: m_entries)
			{
				if (entry.tmpl.shaderVfsPath == shaderVfsPath || entry.tmpl.fragmentVfsPath == shaderVfsPath)
				{
					matching.push_back(entry.tmpl);
					descs.push_back(DescribeFor(entry.tmpl));
				}
			}
		}

		std::vector<PreparedReload> out;
		out.reserve(matching.size());
		for (std::size_t i = 0; i < matching.size(); ++i)
		{
			const MaterialTemplate& tmpl = matching[i];
			gpu::ResourceRegistry::PreparedPipeline prepared = prepare(descs[i]);
			if (prepared.IsValid())
			{
				out.push_back(PreparedReload{tmpl, prepared});
			}
			else
			{
				AE_ERROR(LogCategory::Render, "PipelineCache: reload failed for '{}'; keeping the previous pipeline", tmpl.shaderVfsPath);
			}
		}
		return out;
	}

	std::size_t PipelineCache::CommitReload(std::vector<PreparedReload> prepared)
	{
		const std::scoped_lock lock(m_mutex);
		if (!m_commit)
		{
			return 0;
		}

		std::size_t swapped = 0;
		for (PreparedReload& item: prepared)
		{
			Expected<GraphicsPipeline> built = m_commit(item.prepared);
			if (!built)
			{
				continue;
			}
			// Found again rather than remembered: an entry could in principle have been added
			// since the snapshot, and swapping into a stale address would be a use-after-free.
			bool placed = false;
			auto range = m_entries.equal_range(HashMaterialTemplate(item.tmpl));
			for (auto it = range.first; it != range.second; ++it)
			{
				if (it->second.tmpl == item.tmpl)
				{
					m_retired.push_back(Retired{std::move(it->second.pipeline), m_frameIndex + kPipelineRetireDelayFrames});
					it->second.pipeline = std::move(built.value());
					placed = true;
					++swapped;
					break;
				}
			}
			if (!placed)
			{
				// Nothing to swap into any more; retire it so its shader objects are freed
				// on the normal schedule rather than leaked.
				m_retired.push_back(Retired{std::move(built.value()), m_frameIndex + kPipelineRetireDelayFrames});
			}
		}
		return swapped;
	}

	void PipelineCache::DiscardPrepared(std::vector<PreparedReload> prepared)
	{
		const std::scoped_lock lock(m_mutex);
		if (!m_discard)
		{
			return;
		}
		for (const PreparedReload& item: prepared)
		{
			m_discard(item.prepared);
		}
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
