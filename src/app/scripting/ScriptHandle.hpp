#pragma once

#include <filesystem>
#include <string>

// Forward-declare daScript types to keep this header light.
namespace das
{
	class Context;
	class SimFunction;
} // namespace das

namespace aether::app::scripting
{
	// A compiled daScript and its cached entry-point function pointers.
	// Owned by ScriptingSubsystem; replaced on hot-reload.
	struct ScriptHandle
	{
		std::string scriptPath;

		// Compiled + simulated das context.  Null if compilation failed.
		das::Context* ctx = nullptr;

		// Cached function pointers (null if the exported function is absent).
		das::SimFunction* onAttach = nullptr;
		das::SimFunction* onUpdate = nullptr;
		das::SimFunction* onDetach = nullptr;

		[[nodiscard]] bool IsValid() const
		{
			return ctx != nullptr;
		}
	};
} // namespace aether::app::scripting
