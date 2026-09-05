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

		// Networking: RPCs. GetNetRpcMethod resolves a [NetRpc] method by name (the
		// encode side - a caller building an outbound call turns a method name into
		// the index that goes on the wire, in the type's declaration-order [NetRpc]
		// table) and writes the NetRpcTarget its attribute declared to `outTarget`.
		// Returns -1, leaving `outTarget` untouched, if the type is unknown or
		// declares no such method. InvokeNetRpc is the decode side: it runs method
		// `methodIndex` on the live instance `handle` names. argBlob is a single
		// value - null/empty for a parameterless method, otherwise a UTF-8 string -
		// the one argument shape RPC methods currently support; see NetRpc.hpp.
		// Never throws across the boundary: an unresolvable handle, an out-of-range
		// index, or a managed exception during the call are all swallowed on the
		// managed side and simply produce no call.
		std::int32_t (*GetNetRpcMethod)(const char* typeNameUtf8, const char* methodNameUtf8, std::int32_t* outTarget) = nullptr;
		void (*InvokeNetRpc)(std::uint64_t handle, std::int32_t methodIndex, const std::uint8_t* argBlob, std::int32_t argLen) = nullptr;

		// Compiles a project's scripts in-process, with the Roslyn assemblies shipped beside
		// the engine's managed output. Returns 0 on success; anything else means the build
		// failed and `diagBuf` holds the compiler's own diagnostics, already formatted for a
		// human, truncated to `diagLen`.
		//
		// This is why the editor needs no .NET SDK. `dotnet.exe` ships with the RUNTIME, so
		// asking it to build on a machine that has only that produces "a compatible .NET SDK
		// was not found" - which reached one user as their script failing to compile.
		std::int32_t (*CompileScripts)(const char* scriptDirUtf8, const char* outputPathUtf8, const char* referenceDirUtf8, std::int32_t optimize, char* diagBuf, std::int32_t diagLen) = nullptr;

		// Networking: ownership. Called whenever ownership of a scripted entity becomes
		// KNOWN (offline and on the host that is immediately; on a client it waits for
		// the host's Welcome) or CHANGES HANDS thereafter (e.g. the owning connection
		// disconnecting hands the entity to the host) - see
		// ScriptComponentSystem::DispatchOwnershipChanged for the native half and
		// EntityScript.OnOwnershipChanged for the managed hook this reaches. `owner` is
		// the connection id that owns the entity; `isOwner` is nonzero when the peer
		// running this call is that owner. Appended at the end, like every ABI addition
		// here - reordering an existing slot would desync every already-built managed
		// build from this header without either side's size check noticing why.
		void (*InvokeOwnershipChanged)(std::uint64_t handle, std::uint32_t owner, std::int32_t isOwner) = nullptr;
	};

	// Returns 0 on success; nonzero signals an ABI/version mismatch (the sizes
	using ManagedBootstrapFn = std::int32_t (*)(const NativeHostCallbacks* callbacks, std::int32_t callbacksSize, ManagedScriptApi* outApi, std::int32_t apiSize);
} // namespace aether::scripting
