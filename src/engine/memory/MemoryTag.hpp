#pragma once

#include <cstddef>
#include <cstdint>

namespace aether::memory
{
	// The tag set, as an X-macro so the enum, the count and the name table cannot drift apart.
	// Adding a tag is one line here and nothing else.
	//
	// `Managed` is deliberately absent. Managed memory is not a native allocation and is never
	// attributed by this system; it is surfaced separately from GC statistics, and giving it a
	// MemTag would invite code to "allocate" into it.
#define AE_MEMORY_TAGS(X) \
	X(Unknown)            \
	X(Engine)             \
	X(Rendering)          \
	X(RenderGraph)        \
	X(Vulkan)             \
	X(Mesh)               \
	X(Texture)            \
	X(Material)           \
	X(Shader)             \
	X(Scene)              \
	X(Ecs)                \
	X(Physics3D)          \
	X(Physics2D)          \
	X(Animation)          \
	X(Audio)              \
	X(Scripting)          \
	X(Ui)                 \
	X(Editor)             \
	X(Assets)             \
	X(Io)                 \
	X(Net)                \
	X(Particles)          \
	X(Tilemap)            \
	X(Temp)               \
	X(ThirdParty)

	enum class MemTag : std::uint8_t
	{
#define AE_MEMORY_TAG_ENUM(name) name,
		AE_MEMORY_TAGS(AE_MEMORY_TAG_ENUM)
#undef AE_MEMORY_TAG_ENUM
	};

	inline constexpr std::size_t kMemTagCount = []
	{
		std::size_t count = 0;
#define AE_MEMORY_TAG_COUNT(name) ++count;
		AE_MEMORY_TAGS(AE_MEMORY_TAG_COUNT)
#undef AE_MEMORY_TAG_COUNT
		return count;
	}();

	// Never null, including for a value outside the enum: this is called from the allocation
	// path and from report writers, neither of which can afford to fault on bad input.
	[[nodiscard]] constexpr const char* ToString(const MemTag tag) noexcept
	{
		switch (tag)
		{
#define AE_MEMORY_TAG_NAME(name) \
	case MemTag::name:           \
		return #name;
			AE_MEMORY_TAGS(AE_MEMORY_TAG_NAME)
#undef AE_MEMORY_TAG_NAME
		}
		return "Unknown";
	}
} // namespace aether::memory
