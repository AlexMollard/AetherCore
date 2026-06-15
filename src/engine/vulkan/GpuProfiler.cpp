// Engine-side Tracy GPU profiling facade - implementation.
//
// The pImpl data is defined here, where the Tracy types are fully
// visible. The factory in this same file is the public way to build
// a `ProfilerContextHandle` from a `tracy::VkCtx*`. The
// `reinterpret_cast<VkCommandBuffer>` lives ONLY in this file.

#include "gpu/GpuProfiler.hpp"
#include "vulkan/TracyGpuProfiler.hpp"

#ifdef TRACY_ENABLE
#	include <cstring>

#	include "vulkan/volk.hpp"
#	include <tracy/TracyVulkan.hpp>
#endif

namespace aether::gpu::detail
{
#ifdef TRACY_ENABLE
	// Real definitions of the forward-declared pImpl data. The
	// engine side only sees pointers to these.
	struct ProfilerContextData
	{
		tracy::VkCtx* ctx = nullptr;
	};

	struct ProfilerScopeData
	{
		tracy::VkCtxScope scope;
	};
#endif
} // namespace aether::gpu::detail

namespace aether::vulkan
{
	aether::gpu::ProfilerContextHandle CreateTracyGpuProfilerContext(tracy::VkCtx* ctx) noexcept
	{
#ifdef TRACY_ENABLE
		return new aether::gpu::detail::ProfilerContextData{ .ctx = ctx };
#else
		(void)ctx;
		return nullptr;
#endif
	}

	void DestroyTracyGpuProfilerContext(aether::gpu::ProfilerContextHandle handle) noexcept
	{
#ifdef TRACY_ENABLE
		delete handle;
#else
		(void)handle;
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

	GpuZoneScope GpuProfiler::BeginZoneScopedImpl(gpu::CommandBuffer cmd, std::string_view name,
	                                              const char* file, std::uint32_t line,
	                                              const char* func) noexcept
	{
#ifdef TRACY_ENABLE
		if (m_context == nullptr)
		{
			return GpuZoneScope();
		}
		// Heap-allocate the scope data so the engine-side
		// `GpuZoneScope` can hold it as an opaque typed handle. The
		// dtor writes the end timestamp and queues the end marker,
		// then frees the memory. 5-20 zones per frame at pass-level
		// granularity is well below the cost of Tracy's own per-event
		// queue allocations.
		// `tracy::VkCtxScope` has no default ctor (every ctor is
		// parameterized), so we placement-new in place.
		auto* storage = ::operator new(sizeof(detail::ProfilerScopeData));
		auto* scope_data = reinterpret_cast<detail::ProfilerScopeData*>(storage);
		new (&scope_data->scope) tracy::VkCtxScope(
			m_context->ctx,
			line,
			file,
			std::strlen(file),
			func,
			std::strlen(func),
			name.data(),
			static_cast<std::size_t>(name.size()),
			reinterpret_cast<VkCommandBuffer>(cmd),
			0,    // depth
			true); // is_active
		return GpuZoneScope(scope_data);
#else
		(void)cmd;
		(void)name;
		(void)file;
		(void)line;
		(void)func;
		return GpuZoneScope();
#endif
	}

	void GpuProfiler::Collect(gpu::CommandBuffer cmd) noexcept
	{
#ifdef TRACY_ENABLE
		if (m_context == nullptr)
		{
			return;
		}
		TracyVkCollect(m_context->ctx, reinterpret_cast<VkCommandBuffer>(cmd));
#else
		(void)cmd;
#endif
	}

	void GpuZoneScope::Reset() noexcept
	{
#ifdef TRACY_ENABLE
		if (m_handle != nullptr)
		{
			// Explicitly invoke the dtor (which writes the end
			// timestamp and queues the end marker), then free the
			// heap memory.
			m_handle->scope.~VkCtxScope();
			delete m_handle;
			m_handle = nullptr;
		}
#endif
	}
} // namespace aether::gpu
