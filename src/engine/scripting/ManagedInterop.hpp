#pragma once

#include <cstdint>

// ── Native <-> managed ABI contract ───────────────────────────────────────────

namespace aether::scripting
{
	enum class PropertyType : std::int32_t
	{
		None = 0,
		Float = 1,
		Int = 2,
		Bool = 3,
		Vector3 = 4,
		String = 5,
		Enum = 6,
		Entity = 7,
	};

	struct PropertyValue
	{
		PropertyType type = PropertyType::None;
		std::int32_t reserved = 0;
		float f4[4] = {};
		std::int64_t i64 = 0;
		const char* str = nullptr;
	};

	struct NativeHostCallbacks
	{
		void (*Log)(std::int32_t level, const char* messageUtf8) = nullptr;
		void (*LogAtSource)(std::int32_t level, const char* messageUtf8, const char* filePathUtf8, std::int32_t line) = nullptr;
		void (*ReportScriptError)(const char* messageUtf8) = nullptr;
	};

	struct ManagedScriptApi
	{
		std::int32_t (*LoadScripts)(const char* assemblyPathUtf8) = nullptr;
		void (*UnloadScripts)() = nullptr;
		std::int32_t (*GetScriptTypeCount)() = nullptr;
		std::int32_t (*GetScriptTypeName)(std::int32_t index, char* utf8Buf, std::int32_t bufLen) = nullptr;

		std::uint64_t (*CreateInstance)(const char* typeNameUtf8, std::uint32_t entityId) = nullptr;
		void (*DestroyInstance)(std::uint64_t handle) = nullptr;
		void (*InvokeAttach)(std::uint64_t handle) = nullptr;
		void (*InvokeUpdate)(std::uint64_t handle, float dt) = nullptr;
		void (*InvokeDetach)(std::uint64_t handle) = nullptr;

		std::int32_t (*GetPropertyCount)(const char* typeNameUtf8) = nullptr;
		std::int32_t (*GetPropertyInfo)(const char* typeNameUtf8, std::int32_t index, char* nameBuf, std::int32_t nameBufLen, std::int32_t* outType) = nullptr;
		std::int32_t (*GetProperty)(std::uint64_t handle, std::int32_t index, PropertyValue* outValue) = nullptr;
		std::int32_t (*SetProperty)(std::uint64_t handle, std::int32_t index, const PropertyValue* value) = nullptr;

		void (*SetPlayMode)(std::int32_t playing) = nullptr;
		void (*CollectFull)() = nullptr;

		std::int32_t (*GetDefaultProperty)(const char* typeNameUtf8, std::int32_t index, PropertyValue* outValue) = nullptr;

		void (*DrawEditorWindows)() = nullptr;

		// Project editor-window registry (editor-only). Lets the main editor menu enumerate and toggle
		// whatever IEditorWindow tools the loaded project registered, without knowing any of them.
		std::int32_t (*GetEditorWindowCount)() = nullptr;
		std::int32_t (*GetEditorWindowTitle)(std::int32_t index, char* utf8Buf, std::int32_t bufLen) = nullptr;
		std::int32_t (*GetEditorWindowVisible)(std::int32_t index) = nullptr;
		void (*SetEditorWindowVisible)(std::int32_t index, std::int32_t visible) = nullptr;

		// Networking. Reports which of a script type's properties carry [Replicated],
		// as indices into the property table above; returns the total count and fills
		// at most `maxIndices`. The values themselves still travel through
		// GetProperty/SetProperty, so a replicated script field has exactly one
		// marshalling path.
		std::int32_t (*GetReplicatedPropertyIndices)(const char* typeNameUtf8, std::int32_t* outIndices, std::int32_t maxIndices) = nullptr;
	};

	// Returns 0 on success; nonzero signals an ABI/version mismatch (the sizes
	using ManagedBootstrapFn = std::int32_t (*)(const NativeHostCallbacks* callbacks, std::int32_t callbacksSize, ManagedScriptApi* outApi, std::int32_t apiSize);
} // namespace aether::scripting
