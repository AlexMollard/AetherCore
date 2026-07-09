#pragma once

#include <cstdint>

// ── Native <-> managed ABI contract ───────────────────────────────────────────
//
// These two POD structs are the entire boundary between the C++ host and the C#
// runtime. They are exchanged exactly once, during DotNetHost bootstrap:
//
//   * The host fills a NativeHostCallbacks and passes it into managed Bootstrap.Init.
//   * Bootstrap.Init fills a ManagedScriptApi and returns it to the host.
//
// After that handshake every call in either direction is a raw function-pointer
// call - no reflection, no marshalling layer. The managed side mirrors these
// structs field-for-field as [StructLayout(Sequential)] with `delegate* unmanaged`
// members, so the two must stay in lockstep.
//
// Target is win-x64, where __cdecl/__stdcall collapse to the single x64 calling
// convention, so the pointers carry no explicit calltype and the managed methods
// use a plain [UnmanagedCallersOnly].

namespace aether::scripting
{
	// Wire type tag for a script property value. Mirrors managed PropertyType.
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

	// A single serialized script property crossing the boundary. Blittable
	// tagged union: `type` selects which payload is live. `str` points at a
	// UTF-8 buffer owned by the caller and valid only for the duration of the
	// call.
	struct PropertyValue
	{
		PropertyType type = PropertyType::None;
		std::int32_t reserved = 0; // padding / future flags
		float f4[4] = {};          // Float (x), Vector3 (xyz)
		std::int64_t i64 = 0;      // Int, Bool (0/1), Enum, Entity id
		const char* str = nullptr; // String
	};

	// Callbacks the native host exposes to managed code. Filled by DotNetHost
	// before bootstrap. `level` maps to aether::LogLevel.
	struct NativeHostCallbacks
	{
		void (*Log)(std::int32_t level, const char* messageUtf8) = nullptr;
		void (*LogAtSource)(std::int32_t level, const char* messageUtf8, const char* filePathUtf8, std::int32_t line) = nullptr;
		void (*ReportScriptError)(const char* messageUtf8) = nullptr;
	};

	// The managed script runtime API. Filled by Bootstrap.Init, consumed by
	// CSharpScriptingSubsystem / ScriptComponentSystem. Function pointers that a
	// given phase has not implemented yet are left null; the host only calls the
	// ones it needs.
	struct ManagedScriptApi
	{
		// ── Assembly / registry lifecycle ────────────────────────────────────
		// Loads the game-scripts assembly into a fresh collectible load context.
		// Returns the number of discovered EntityScript types, or -1 on failure.
		std::int32_t (*LoadScripts)(const char* assemblyPathUtf8) = nullptr;
		// Detaches everything and unloads the previous collectible context.
		void (*UnloadScripts)() = nullptr;
		std::int32_t (*GetScriptTypeCount)() = nullptr;
		// Writes the type name at `index` into `utf8Buf`; returns bytes written.
		std::int32_t (*GetScriptTypeName)(std::int32_t index, char* utf8Buf, std::int32_t bufLen) = nullptr;

		// ── Per-entity instance lifecycle ────────────────────────────────────
		// handle == GCHandle.ToIntPtr as u64; 0 == failure.
		std::uint64_t (*CreateInstance)(const char* typeNameUtf8, std::uint32_t entityId) = nullptr;
		void (*DestroyInstance)(std::uint64_t handle) = nullptr;
		void (*InvokeAttach)(std::uint64_t handle) = nullptr;
		void (*InvokeUpdate)(std::uint64_t handle, float dt) = nullptr;
		void (*InvokeDetach)(std::uint64_t handle) = nullptr;

		// ── Serialized script properties (inspector / scene overrides) ───────
		std::int32_t (*GetPropertyCount)(const char* typeNameUtf8) = nullptr;
		std::int32_t (*GetPropertyInfo)(const char* typeNameUtf8, std::int32_t index, char* nameBuf, std::int32_t nameBufLen, std::int32_t* outType) = nullptr;
		std::int32_t (*GetProperty)(std::uint64_t handle, std::int32_t index, PropertyValue* outValue) = nullptr;
		std::int32_t (*SetProperty)(std::uint64_t handle, std::int32_t index, const PropertyValue* value) = nullptr;

		// ── GC policy ────────────────────────────────────────────────────────
		void (*SetPlayMode)(std::int32_t playing) = nullptr; // SustainedLowLatency <-> Interactive
		void (*CollectFull)() = nullptr;

		// Reads a type's default field value from a cached default instance (for
		// the inspector when no live instance exists).
		std::int32_t (*GetDefaultProperty)(const char* typeNameUtf8, std::int32_t index, PropertyValue* outValue) = nullptr;
	};

	// Signature of the managed Bootstrap.Init entry point resolved via
	// load_assembly_and_get_function_pointer with UNMANAGEDCALLERSONLY_METHOD.
	// Returns 0 on success; nonzero signals an ABI/version mismatch (the sizes
	// let managed reject a host built against a different struct layout).
	using ManagedBootstrapFn = std::int32_t (*)(const NativeHostCallbacks* callbacks, std::int32_t callbacksSize, ManagedScriptApi* outApi, std::int32_t apiSize);
} // namespace aether::scripting
