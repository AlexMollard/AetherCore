// pImpl data and `reinterpret_cast<VkCommandBuffer>` live in this file.

#include "gpu/GpuProfiler.hpp"
#include "vulkan/TracyGpuProfiler.hpp"

#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
#	include <cstring>

#	include "vulkan/volk.hpp"
#	include <tracy/TracyVulkan.hpp>
#endif

namespace aether::gpu::detail
{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
	struct ProfilerContextData
	{
		tracy::VkCtx* ctx = nullptr;
	};

	struct ProfilerScopeData
	{
		ProfilerScopeData(tracy::VkCtx* ctx, std::uint32_t line, const char* file, std::size_t fileSize, const char* func, std::size_t funcSize, const char* name, std::size_t nameSize, VkCommandBuffer cmd)
		      : scope(ctx, line, file, fileSize, func, funcSize, name, nameSize, cmd, 0, true)
		{
		}

		tracy::VkCtxScope scope;
	};
#endif
} // namespace aether::gpu::detail

namespace aether::vulkan
{
	aether::gpu::ProfilerContextHandle CreateTracyGpuProfilerContext(tracy::VkCtx* ctx) noexcept
	{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		return new aether::gpu::detail::ProfilerContextData{.ctx = ctx};
#else
		(void) ctx;
		return nullptr;
#endif
	}

	void DestroyTracyGpuProfilerContext(aether::gpu::ProfilerContextHandle handle) noexcept
	{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		delete handle;
#else
		(void) handle;
#endif
	}
} // namespace aether::vulkan

namespace aether::gpu
{
	GpuProfiler& GpuProfiler::Get() noexcept
	{
		static GpuProfiler instance;
		return instance;
	}

	void GpuProfiler::Initialize(const GpuProfilerInit& init) noexcept
	{
		m_context = init.context;
	}

	void GpuProfiler::Shutdown() noexcept
	{
		m_context = nullptr;
	}

	void GpuProfiler::SetName(std::string_view name) noexcept
	{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		if (m_context == nullptr)
		{
			return;
		}
		TracyVkContextName(m_context->ctx, name.data(), static_cast<std::uint16_t>(name.size()));
#else
		(void) name;
#endif
	}

	GpuZoneScope GpuProfiler::BeginZoneScopedImpl(gpu::CommandBuffer cmd, std::string_view name, const char* file, std::uint32_t line, const char* func) noexcept
	{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		if (m_context == nullptr)
		{
			return {};
		}
		// Heap-allocate scope data so the engine-side GpuZoneScope can hold it as an opaque typed handle.
		auto scopeData = new detail::ProfilerScopeData(m_context->ctx,
		        line,
		        file,
		        std::strlen(file),
		        func,
		        std::strlen(func),
		        name.data(),
		        static_cast<std::size_t>(name.size()),
		        reinterpret_cast<VkCommandBuffer>(cmd));
		return GpuZoneScope(scopeData);
#else
		(void) cmd;
		(void) name;
		(void) file;
		(void) line;
		(void) func;
		return GpuZoneScope();
#endif
	}

	void GpuProfiler::Collect(gpu::CommandBuffer cmd) noexcept
	{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		if (m_context == nullptr)
		{
			return;
		}
		TracyVkCollect(m_context->ctx, reinterpret_cast<VkCommandBuffer>(cmd));
#else
		(void) cmd;
#endif
	}

	void GpuZoneScope::Reset() noexcept
	{
#if defined(TRACY_ENABLE) && AETHERCORE_ENABLE_TRACY_GPU
		if (m_handle != nullptr)
		{
			// Explicit dtor call writes the end timestamp and queues the end marker.
			delete m_handle;
			m_handle = nullptr;
		}
#endif
	}
} // namespace aether::gpu
