#include "vulkan/GpuEnumConversions.hpp"

#include "vulkan/volk.hpp"

namespace aether::gpu
{
	// All Format enum values are enumerated explicitly to satisfy
	// -Werror=switch-enum. Anything the engine does not (yet) use maps to
	// VK_FORMAT_UNDEFINED. Extending Format will force a compile error here,
	// prompting the engineer to decide on the VkFormat.
	VkFormat ToVk(Format format) noexcept
	{
		switch (format)
		{
			case Format::Undefined:
				return VK_FORMAT_UNDEFINED;
			case Format::R8Unorm:
				return VK_FORMAT_R8_UNORM;
			case Format::R8G8B8A8Unorm:
				return VK_FORMAT_R8G8B8A8_UNORM;
			case Format::R8G8B8A8Srgb:
				return VK_FORMAT_R8G8B8A8_SRGB;
			case Format::B8G8R8A8Unorm:
				return VK_FORMAT_B8G8R8A8_UNORM;
			case Format::B8G8R8A8Srgb:
				return VK_FORMAT_B8G8R8A8_SRGB;
			case Format::R16G16B16A16Sfloat:
				return VK_FORMAT_R16G16B16A16_SFLOAT;
			case Format::R32G32Sfloat:
				return VK_FORMAT_R32G32_SFLOAT;
			case Format::R32G32B32Sfloat:
				return VK_FORMAT_R32G32B32_SFLOAT;
			case Format::R32G32B32A32Sfloat:
				return VK_FORMAT_R32G32B32A32_SFLOAT;
			case Format::D16Unorm:
				return VK_FORMAT_D16_UNORM;
			case Format::D32Sfloat:
				return VK_FORMAT_D32_SFLOAT;
			case Format::D24UnormS8Uint:
				return VK_FORMAT_D24_UNORM_S8_UINT;
			case Format::X8D24UnormPack32:
				return VK_FORMAT_X8_D24_UNORM_PACK32;
			case Format::D16UnormS8Uint:
				return VK_FORMAT_D16_UNORM_S8_UINT;
			case Format::D32SfloatS8Uint:
				return VK_FORMAT_D32_SFLOAT_S8_UINT;
			case Format::BC4UnormBlock:
				return VK_FORMAT_BC4_UNORM_BLOCK;
			case Format::BC7UnormBlock:
				return VK_FORMAT_BC7_UNORM_BLOCK;
			case Format::BC7SrgbBlock:
				return VK_FORMAT_BC7_SRGB_BLOCK;
		}
		return VK_FORMAT_UNDEFINED;
	}

	// VkFormat has ~250 entries and the engine only uses ~20 of them. The
	// project's existing pattern (see GpuDevice.cpp) is to silence -Wswitch-enum
	// for the VkFormat switch and funnel unknown values through Format::Undefined
	// via a default arm. This is preferable to enumerating hundreds of cases
	// that always return Undefined.
	Format FromVk(VkFormat format) noexcept
	{
#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wswitch-enum"
#endif
		switch (format)
		{
			case VK_FORMAT_R8_UNORM:
				return Format::R8Unorm;
			case VK_FORMAT_R8G8B8A8_UNORM:
				return Format::R8G8B8A8Unorm;
			case VK_FORMAT_R8G8B8A8_SRGB:
				return Format::R8G8B8A8Srgb;
			case VK_FORMAT_B8G8R8A8_UNORM:
				return Format::B8G8R8A8Unorm;
			case VK_FORMAT_B8G8R8A8_SRGB:
				return Format::B8G8R8A8Srgb;
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return Format::R16G16B16A16Sfloat;
			case VK_FORMAT_R32G32_SFLOAT:
				return Format::R32G32Sfloat;
			case VK_FORMAT_R32G32B32_SFLOAT:
				return Format::R32G32B32Sfloat;
			case VK_FORMAT_R32G32B32A32_SFLOAT:
				return Format::R32G32B32A32Sfloat;
			case VK_FORMAT_D16_UNORM:
				return Format::D16Unorm;
			case VK_FORMAT_D32_SFLOAT:
				return Format::D32Sfloat;
			case VK_FORMAT_D24_UNORM_S8_UINT:
				return Format::D24UnormS8Uint;
			case VK_FORMAT_X8_D24_UNORM_PACK32:
				return Format::X8D24UnormPack32;
			case VK_FORMAT_D16_UNORM_S8_UINT:
				return Format::D16UnormS8Uint;
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return Format::D32SfloatS8Uint;
			case VK_FORMAT_BC4_UNORM_BLOCK:
				return Format::BC4UnormBlock;
			case VK_FORMAT_BC7_UNORM_BLOCK:
				return Format::BC7UnormBlock;
			case VK_FORMAT_BC7_SRGB_BLOCK:
				return Format::BC7SrgbBlock;
			case VK_FORMAT_UNDEFINED:
				return Format::Undefined;
			default:
				return Format::Undefined;
		}
#ifdef __clang__
#	pragma clang diagnostic pop
#endif
	}

	bool IsDepthFormat(Format format) noexcept
	{
		switch (format)
		{
			case Format::D16Unorm:
			case Format::D32Sfloat:
			case Format::D24UnormS8Uint:
			case Format::X8D24UnormPack32:
			case Format::D16UnormS8Uint:
			case Format::D32SfloatS8Uint:
				return true;
			case Format::Undefined:
			case Format::R8Unorm:
			case Format::R8G8B8A8Unorm:
			case Format::R8G8B8A8Srgb:
			case Format::B8G8R8A8Unorm:
			case Format::B8G8R8A8Srgb:
			case Format::R16G16B16A16Sfloat:
			case Format::R32G32Sfloat:
			case Format::R32G32B32Sfloat:
			case Format::R32G32B32A32Sfloat:
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
				return false;
		}
		return false;
	}

	bool IsStencilFormat(Format format) noexcept
	{
		switch (format)
		{
			case Format::D24UnormS8Uint:
			case Format::D16UnormS8Uint:
			case Format::D32SfloatS8Uint:
				return true;
			case Format::Undefined:
			case Format::R8Unorm:
			case Format::R8G8B8A8Unorm:
			case Format::R8G8B8A8Srgb:
			case Format::B8G8R8A8Unorm:
			case Format::B8G8R8A8Srgb:
			case Format::R16G16B16A16Sfloat:
			case Format::R32G32Sfloat:
			case Format::R32G32B32Sfloat:
			case Format::R32G32B32A32Sfloat:
			case Format::D16Unorm:
			case Format::D32Sfloat:
			case Format::X8D24UnormPack32:
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
				return false;
		}
		return false;
	}

	bool IsBlockCompressed(Format format) noexcept
	{
		switch (format)
		{
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
				return true;
			case Format::Undefined:
			case Format::R8Unorm:
			case Format::R8G8B8A8Unorm:
			case Format::R8G8B8A8Srgb:
			case Format::B8G8R8A8Unorm:
			case Format::B8G8R8A8Srgb:
			case Format::R16G16B16A16Sfloat:
			case Format::R32G32Sfloat:
			case Format::R32G32B32Sfloat:
			case Format::R32G32B32A32Sfloat:
			case Format::D16Unorm:
			case Format::D32Sfloat:
			case Format::D24UnormS8Uint:
			case Format::X8D24UnormPack32:
			case Format::D16UnormS8Uint:
			case Format::D32SfloatS8Uint:
				return false;
		}
		return false;
	}

	std::uint32_t BytesPerPixel(Format format) noexcept
	{
		// Block-compressed formats report 0 here - call sites that need
		// exact block-size accounting must use a dedicated BCn helper.
		switch (format)
		{
			case Format::R8Unorm:
				return 1u;
			case Format::R8G8B8A8Unorm:
				return 4u;
			case Format::R8G8B8A8Srgb:
				return 4u;
			case Format::B8G8R8A8Unorm:
				return 4u;
			case Format::B8G8R8A8Srgb:
				return 4u;
			case Format::R16G16B16A16Sfloat:
				return 8u;
			case Format::R32G32Sfloat:
				return 8u;
			case Format::R32G32B32Sfloat:
				return 12u;
			case Format::R32G32B32A32Sfloat:
				return 16u;
			case Format::D16Unorm:
				return 2u;
			case Format::D32Sfloat:
				return 4u;
			case Format::D24UnormS8Uint:
				return 4u;
			case Format::X8D24UnormPack32:
				return 4u;
			case Format::D16UnormS8Uint:
				return 4u;
			case Format::D32SfloatS8Uint:
				return 8u;
			case Format::BC4UnormBlock:
			case Format::BC7UnormBlock:
			case Format::BC7SrgbBlock:
			case Format::Undefined:
				return 0u;
		}
		return 0u;
	}

	VkPipelineBindPoint ToVk(PipelineBindPoint bindPoint) noexcept
	{
		switch (bindPoint)
		{
			case PipelineBindPoint::Graphics:
				return VK_PIPELINE_BIND_POINT_GRAPHICS;
			case PipelineBindPoint::Compute:
				return VK_PIPELINE_BIND_POINT_COMPUTE;
		}
		return VK_PIPELINE_BIND_POINT_GRAPHICS;
	}

	// Bit values mirror VkPipelineStageFlagBits2 / VkAccessFlagBits2 directly,
	// so the engine-side enum can forward to the Vk* bitmask without any
	// per-bit translation. Unknown / unsupported stage bits fall through to
	// 0 (no stage), which makes any misuse a no-op rather than a crash.
	VkPipelineStageFlags2 ToVk(PipelineStage stage) noexcept
	{
		return static_cast<VkPipelineStageFlags2>(stage);
	}

	VkAccessFlags2 ToVk(AccessFlags access) noexcept
	{
		return static_cast<VkAccessFlags2>(access);
	}

	VkDescriptorType ToVk(DescriptorType type) noexcept
	{
		switch (type)
		{
			case DescriptorType::Sampler:
				return VK_DESCRIPTOR_TYPE_SAMPLER;
			case DescriptorType::CombinedImageSampler:
				return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case DescriptorType::SampledImage:
				return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			case DescriptorType::StorageImage:
				return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case DescriptorType::UniformTexelBuffer:
				return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
			case DescriptorType::StorageTexelBuffer:
				return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
			case DescriptorType::StorageBuffer:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case DescriptorType::UniformBuffer:
				return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case DescriptorType::UniformBufferDynamic:
				return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			case DescriptorType::StorageBufferDynamic:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
			case DescriptorType::InputAttachment:
				return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		}
		return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	}

	VkShaderStageFlags ToVk(ShaderStage stage) noexcept
	{
		VkShaderStageFlags out = 0;
		if ((static_cast<std::uint32_t>(stage) & static_cast<std::uint32_t>(ShaderStage::Vertex)) != 0)
		{
			out |= VK_SHADER_STAGE_VERTEX_BIT;
		}
		if ((static_cast<std::uint32_t>(stage) & static_cast<std::uint32_t>(ShaderStage::Fragment)) != 0)
		{
			out |= VK_SHADER_STAGE_FRAGMENT_BIT;
		}
		if ((static_cast<std::uint32_t>(stage) & static_cast<std::uint32_t>(ShaderStage::Compute)) != 0)
		{
			out |= VK_SHADER_STAGE_COMPUTE_BIT;
		}
		return out;
	}

	// -------------------------------------------------------------------------
	// DescriptorSetLayoutFlags
	// -------------------------------------------------------------------------
	VkDescriptorSetLayoutCreateFlags ToVk(DescriptorSetLayoutFlags flags) noexcept
	{
		VkDescriptorSetLayoutCreateFlags out = 0;
		if ((static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(DescriptorSetLayoutFlags::PushDescriptor)) != 0)
		{
			out |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
		}
		return out;
	}

	VkCompareOp ToVk(CompareOp op) noexcept
	{
		switch (op)
		{
			case CompareOp::Never:
				return VK_COMPARE_OP_NEVER;
			case CompareOp::Less:
				return VK_COMPARE_OP_LESS;
			case CompareOp::Equal:
				return VK_COMPARE_OP_EQUAL;
			case CompareOp::LessOrEqual:
				return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Greater:
				return VK_COMPARE_OP_GREATER;
			case CompareOp::NotEqual:
				return VK_COMPARE_OP_NOT_EQUAL;
			case CompareOp::GreaterOrEqual:
				return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Always:
				return VK_COMPARE_OP_ALWAYS;
		}
		return VK_COMPARE_OP_LESS;
	}

	VkAttachmentLoadOp ToVk(LoadOp op) noexcept
	{
		switch (op)
		{
			case LoadOp::Load:
				return VK_ATTACHMENT_LOAD_OP_LOAD;
			case LoadOp::Clear:
				return VK_ATTACHMENT_LOAD_OP_CLEAR;
			case LoadOp::DontCare:
				return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		}
		return VK_ATTACHMENT_LOAD_OP_CLEAR;
	}

	VkAttachmentStoreOp ToVk(StoreOp op) noexcept
	{
		switch (op)
		{
			case StoreOp::Store:
				return VK_ATTACHMENT_STORE_OP_STORE;
			case StoreOp::DontCare:
				return VK_ATTACHMENT_STORE_OP_DONT_CARE;
		}
		return VK_ATTACHMENT_STORE_OP_STORE;
	}

	VkImageUsageFlags ToVk(ImageUsage usage) noexcept
	{
		VkImageUsageFlags out = 0;
		const auto bits = static_cast<std::uint32_t>(usage);
		if ((bits & static_cast<std::uint32_t>(ImageUsage::TransferSrc)) != 0)
		{
			out |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageUsage::TransferDst)) != 0)
		{
			out |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageUsage::Sampled)) != 0)
		{
			out |= VK_IMAGE_USAGE_SAMPLED_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageUsage::Storage)) != 0)
		{
			out |= VK_IMAGE_USAGE_STORAGE_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageUsage::ColorAttachment)) != 0)
		{
			out |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageUsage::DepthStencilAttachment)) != 0)
		{
			out |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		return out;
	}

	VkBufferUsageFlags ToVk(BufferUsage usage) noexcept
	{
		VkBufferUsageFlags out = 0;
		const auto bits = static_cast<std::uint32_t>(usage);
		if ((bits & static_cast<std::uint32_t>(BufferUsage::TransferSrc)) != 0)
		{
			out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::TransferDst)) != 0)
		{
			out |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::UniformTexel)) != 0)
		{
			out |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::StorageTexel)) != 0)
		{
			out |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::Uniform)) != 0)
		{
			out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::Storage)) != 0)
		{
			out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::Index)) != 0)
		{
			out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::Vertex)) != 0)
		{
			out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::Indirect)) != 0)
		{
			out |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(BufferUsage::ShaderDeviceAddress)) != 0)
		{
			out |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		}
		return out;
	}

	VkImageAspectFlags ToVk(ImageAspect aspect) noexcept
	{
		VkImageAspectFlags out = 0;
		const auto bits = static_cast<std::uint32_t>(aspect);
		if ((bits & static_cast<std::uint32_t>(ImageAspect::Color)) != 0)
		{
			out |= VK_IMAGE_ASPECT_COLOR_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageAspect::Depth)) != 0)
		{
			out |= VK_IMAGE_ASPECT_DEPTH_BIT;
		}
		if ((bits & static_cast<std::uint32_t>(ImageAspect::Stencil)) != 0)
		{
			out |= VK_IMAGE_ASPECT_STENCIL_BIT;
		}
		return out;
	}

	ImageAspect FromVk(VkImageAspectFlags aspect) noexcept
	{
		std::uint32_t bits = 0;
		if ((aspect & VK_IMAGE_ASPECT_COLOR_BIT) != 0)
		{
			bits |= static_cast<std::uint32_t>(ImageAspect::Color);
		}
		if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
		{
			bits |= static_cast<std::uint32_t>(ImageAspect::Depth);
		}
		if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0)
		{
			bits |= static_cast<std::uint32_t>(ImageAspect::Stencil);
		}
		return static_cast<ImageAspect>(bits);
	}

	VkImageLayout ToVk(ImageLayout layout) noexcept
	{
		switch (layout)
		{
			case ImageLayout::Undefined:
				return VK_IMAGE_LAYOUT_UNDEFINED;
			case ImageLayout::General:
				return VK_IMAGE_LAYOUT_GENERAL;
			case ImageLayout::ColorAttachment:
				return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case ImageLayout::DepthAttachment:
				return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
			case ImageLayout::ShaderReadOnly:
				return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		return VK_IMAGE_LAYOUT_UNDEFINED;
	}

	ImageLayout FromVk(VkImageLayout layout) noexcept
	{
		switch (layout)
		{
			case VK_IMAGE_LAYOUT_UNDEFINED:
				return ImageLayout::Undefined;
			case VK_IMAGE_LAYOUT_GENERAL:
				return ImageLayout::General;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				return ImageLayout::ColorAttachment;
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
				return ImageLayout::DepthAttachment;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				return ImageLayout::ShaderReadOnly;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			case VK_IMAGE_LAYOUT_PREINITIALIZED:
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			case VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR:
			case VK_IMAGE_LAYOUT_VIDEO_DECODE_SRC_KHR:
			case VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR:
			case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DST_KHR:
			case VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR:
			case VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR:
			case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
			case VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT:
			case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
			case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR:
			case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR:
			case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
			case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
			case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
			case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
			case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
			case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ:
			case VK_IMAGE_LAYOUT_TENSOR_ALIASING_ARM:
			case VK_IMAGE_LAYOUT_VIDEO_ENCODE_QUANTIZATION_MAP_KHR:
			case VK_IMAGE_LAYOUT_ZERO_INITIALIZED_EXT:
			case VK_IMAGE_LAYOUT_MAX_ENUM:
				return ImageLayout::Undefined;
		}
		return ImageLayout::Undefined;
	}

	VkComponentSwizzle ToVk(ComponentSwizzle s) noexcept
	{
		switch (s)
		{
			case ComponentSwizzle::Identity:
				return VK_COMPONENT_SWIZZLE_IDENTITY;
			case ComponentSwizzle::Zero:
				return VK_COMPONENT_SWIZZLE_ZERO;
			case ComponentSwizzle::One:
				return VK_COMPONENT_SWIZZLE_ONE;
			case ComponentSwizzle::R:
				return VK_COMPONENT_SWIZZLE_R;
			case ComponentSwizzle::G:
				return VK_COMPONENT_SWIZZLE_G;
			case ComponentSwizzle::B:
				return VK_COMPONENT_SWIZZLE_B;
			case ComponentSwizzle::A:
				return VK_COMPONENT_SWIZZLE_A;
		}
		return VK_COMPONENT_SWIZZLE_IDENTITY;
	}

	VkFilter ToVk(Filter filter) noexcept
	{
		switch (filter)
		{
			case Filter::Nearest:
				return VK_FILTER_NEAREST;
			case Filter::Linear:
				return VK_FILTER_LINEAR;
		}
		return VK_FILTER_NEAREST;
	}

	VkSamplerMipmapMode ToVk(SamplerMipmapMode mode) noexcept
	{
		switch (mode)
		{
			case SamplerMipmapMode::Nearest:
				return VK_SAMPLER_MIPMAP_MODE_NEAREST;
			case SamplerMipmapMode::Linear:
				return VK_SAMPLER_MIPMAP_MODE_LINEAR;
		}
		return VK_SAMPLER_MIPMAP_MODE_NEAREST;
	}

	VkSamplerAddressMode ToVk(SamplerAddressMode mode) noexcept
	{
		switch (mode)
		{
			case SamplerAddressMode::Repeat:
				return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case SamplerAddressMode::MirroredRepeat:
				return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case SamplerAddressMode::ClampToEdge:
				return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case SamplerAddressMode::ClampToBorder:
				return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
		}
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	}

	VkClearValue ToVk(const ClearValue& value) noexcept
	{
		VkClearValue v{};
		v.color.float32[0] = value.color[0];
		v.color.float32[1] = value.color[1];
		v.color.float32[2] = value.color[2];
		v.color.float32[3] = value.color[3];
		v.depthStencil.depth = value.depth;
		v.depthStencil.stencil = value.stencil;
		return v;
	}

	VkPrimitiveTopology ToVk(PrimitiveTopology topology) noexcept
	{
		switch (topology)
		{
			case PrimitiveTopology::PointList:
				return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case PrimitiveTopology::LineList:
				return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case PrimitiveTopology::TriangleList:
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}

	// -------------------------------------------------------------------------
	// PolygonMode
	// -------------------------------------------------------------------------
	VkPolygonMode ToVk(PolygonMode mode) noexcept
	{
		switch (mode)
		{
			case PolygonMode::Fill:
				return VK_POLYGON_MODE_FILL;
			case PolygonMode::Line:
				return VK_POLYGON_MODE_LINE;
		}
		return VK_POLYGON_MODE_FILL;
	}

	// The engine-side barrier structs hold opaque gpu::Image / gpu::Buffer
	// handles; the storage knows the actual VkImage / VkBuffer to plug in.
	// The translation here is otherwise a one-for-one field copy.

	VkImageMemoryBarrier2 ToVk(const ImageMemoryBarrier& barrier, VkImage resolvedImage) noexcept
	{
		return VkImageMemoryBarrier2{
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		        .srcStageMask = ToVk(barrier.srcStage),
		        .srcAccessMask = ToVk(barrier.srcAccess),
		        .dstStageMask = ToVk(barrier.dstStage),
		        .dstAccessMask = ToVk(barrier.dstAccess),
		        .oldLayout = ToVk(barrier.oldLayout),
		        .newLayout = ToVk(barrier.newLayout),
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = resolvedImage,
		        .subresourceRange =
		                {
		                        .aspectMask = ToVk(barrier.aspect),
		                        .baseMipLevel = barrier.baseMipLevel,
		                        .levelCount = barrier.levelCount,
		                        .baseArrayLayer = barrier.baseArrayLayer,
		                        .layerCount = barrier.layerCount,
		                },
		};
	}

	VkBufferMemoryBarrier2 ToVk(const BufferMemoryBarrier& barrier, VkBuffer resolvedBuffer) noexcept
	{
		return VkBufferMemoryBarrier2{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		        .srcStageMask = ToVk(barrier.srcStage),
		        .srcAccessMask = ToVk(barrier.srcAccess),
		        .dstStageMask = ToVk(barrier.dstStage),
		        .dstAccessMask = ToVk(barrier.dstAccess),
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer = resolvedBuffer,
		        .offset = barrier.offset,
		        .size = barrier.size,
		};
	}

	VkRenderingAttachmentInfo ToVk(const RenderingAttachmentInfo& info) noexcept
	{
		return VkRenderingAttachmentInfo{
		        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		        .imageView = static_cast<VkImageView>(info.imageView),
		        .imageLayout = ToVk(info.imageLayout),
		        .loadOp = ToVk(info.loadOp),
		        .storeOp = ToVk(info.storeOp),
		        .clearValue = ToVk(info.clearValue),
		};
	}

	VkRenderingInfo ToVk(const RenderingInfo& info, const VkRenderingAttachmentInfo* vkColorAttachments, std::uint32_t colorAttachmentCount, const VkRenderingAttachmentInfo* vkDepthAttachment) noexcept
	{
		return VkRenderingInfo{
		        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = {.width = info.width, .height = info.height}},
		        .layerCount = info.layerCount,
		        .colorAttachmentCount = colorAttachmentCount,
		        .pColorAttachments = vkColorAttachments,
		        .pDepthAttachment = vkDepthAttachment,
		};
	}
} // namespace aether::gpu
