#include "gpu/ResourceRegistry.hpp"

#include "utils/Assert.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace
{
	::aether::ResourceRegistry* s_reg = nullptr;
}

namespace aether::gpu
{
	void ResourceRegistry::Initialize(const ResourceRegistryInitDesc& desc) noexcept
	{
		s_reg = static_cast<::aether::ResourceRegistry*>(desc.backendRegistry);
		AE_ASSERT(s_reg != nullptr, "ResourceRegistry forwarding pointer is null.");
	}

	BufferHandle ResourceRegistry::CreateBuffer(const BufferDesc& d) noexcept
	{
		return s_reg->CreateBuffer(d);
	}

	BufferHandle ResourceRegistry::CreateMappedBuffer(const MappedBufferDesc& d) noexcept
	{
		return s_reg->CreateMappedBuffer(d);
	}

	TextureHandle ResourceRegistry::CreateTexture(const TextureDesc& d) noexcept
	{
		return s_reg->CreateTexture(d);
	}

	MappedBufferView ResourceRegistry::ResolveMappedBuffer(BufferHandle h) noexcept
	{
		return s_reg->ResolveMappedBuffer(h);
	}

	void ResourceRegistry::FlushMappedBuffer(BufferHandle h, DeviceSize o, DeviceSize s) noexcept
	{
		s_reg->FlushMappedBuffer(h, o, s);
	}

	void ResourceRegistry::Destroy(BufferHandle h) noexcept
	{
		s_reg->Destroy(h);
	}

	void ResourceRegistry::Destroy(TextureHandle h) noexcept
	{
		s_reg->Destroy(h);
	}
} // namespace aether::gpu
