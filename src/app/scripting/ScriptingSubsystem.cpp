#include "ScriptingSubsystem.hpp"
#include "ScriptHandle.hpp"
#include "SceneContext.hpp"

#include "modules/WorldModule.hpp"
#include "modules/RendererModule.hpp"
#include "modules/CameraModule.hpp"
#include "modules/InputModule.hpp"
#include "modules/SystemsModule.hpp"

// Pull in the full daScript API after our own headers (heavy include).
#include "daScript/daScript.h"
#include "daScript/simulate/fs_file_info.h"

#include "utils/Logger.hpp"

#include <filesystem>

// ── Module registration ────────────────────────────────────────────────────────
// NEED_MODULE and REGISTER_MODULE use ## token-pasting so they must be called at
// global scope with unqualified class names.  All RegisterXModule() functions are
// defined at global scope in their respective .cpp files.

static void EnsureModulesRegistered()
{
	NEED_ALL_DEFAULT_MODULES;
	RegisterWorldModule();
	RegisterRendererModule();
	RegisterCameraModule();
	RegisterInputModule();
	RegisterSystemsModule();
	das::Module::Initialize();
}

// ── ScriptingSubsystem ─────────────────────────────────────────────────────────

namespace aether::app::scripting
{
	ScriptingSubsystem::ScriptingSubsystem()
	{
		if (!m_modulesRegistered)
		{
			EnsureModulesRegistered();
			m_modulesRegistered = true;
		}
		INFO(LogCategory::App, "ScriptingSubsystem initialised.");
	}

	ScriptingSubsystem::~ScriptingSubsystem()
	{
		das::Module::Shutdown();
	}

	void ScriptingSubsystem::FreeHandle(ScriptHandle& handle)
	{
		delete handle.ctx;
		handle = {};
	}

	// Walk candidate roots to find the script file.
	// In dev builds AETHER_SCRIPTS_SOURCE_DIR is defined to the source resources/scenes
	// directory, so hot-reload (F5) reads from the source tree without a C++ rebuild.
	static std::string ResolveScriptPath(const std::string& path)
	{
		const std::filesystem::path rel(path);
		if (rel.is_absolute())
		{
			return path;
		}

		const auto cwd = std::filesystem::current_path();

		// Build candidate list.  Source dir is tried first so live edits are picked up.
		std::vector<std::filesystem::path> roots;
#ifdef AETHER_SCRIPTS_SOURCE_DIR
		roots.emplace_back(AETHER_SCRIPTS_SOURCE_DIR);
#endif
		roots.emplace_back(cwd / "data" / "scenes");
		roots.emplace_back(cwd / "../data/scenes");
		roots.emplace_back(cwd / "../../data/scenes");
		// Also try the full relative path as-given from cwd and parents.
		roots.emplace_back(cwd);
		roots.emplace_back(cwd / "..");
		roots.emplace_back(cwd / "../..");

		for (const auto& base: roots)
		{
			std::error_code ec;
			const auto candidate = std::filesystem::weakly_canonical(base / rel, ec);
			if (!ec && std::filesystem::exists(candidate, ec))
			{
				return candidate.string();
			}
		}
		return path; // fall back to as-given; let daScript report the error
	}

	ScriptHandle ScriptingSubsystem::Compile(const std::string& path)
	{
		m_lastError.clear();
		const std::string resolvedPath = ResolveScriptPath(path);

		auto fAccess = das::make_smart<das::FsFileAccess>();
		das::ModuleGroup moduleGroup;
		das::TextPrinter logs;

		auto program = das::compileDaScript(resolvedPath, fAccess, logs, moduleGroup);
		if (!program || program->failed())
		{
			m_lastError = "Compile error in " + resolvedPath + ":\n";
			for (const auto& err: program ? program->errors : decltype(program->errors){})
			{
				m_lastError += das::reportError(err.at, err.what, err.extra, err.fixme, err.cerr);
				m_lastError += '\n';
			}
			if (program && program->errors.empty())
			{
				m_lastError += "(unknown compile error)";
			}
			ERROR(LogCategory::App, "ScriptingSubsystem: {}", m_lastError);
			return {};
		}

		auto ctx = new das::Context(program->getContextStackSize());
		das::TextPrinter simLogs;
		if (!program->simulate(*ctx, simLogs))
		{
			m_lastError = "Simulate error in " + resolvedPath + ":\n";
			for (const auto& err: program->errors)
			{
				m_lastError += das::reportError(err.at, err.what, err.extra, err.fixme, err.cerr);
				m_lastError += '\n';
			}
			ERROR(LogCategory::App, "ScriptingSubsystem: {}", m_lastError);
			delete ctx;
			return {};
		}

		ScriptHandle handle;
		handle.scriptPath = resolvedPath;
		handle.ctx = ctx;
		handle.onAttach = ctx->findFunction("on_attach");
		handle.onUpdate = ctx->findFunction("on_update");
		handle.onDetach = ctx->findFunction("on_detach");

		INFO(LogCategory::App, "Script compiled: '{}' (attach={} update={} detach={})", resolvedPath, handle.onAttach != nullptr, handle.onUpdate != nullptr, handle.onDetach != nullptr);

		return handle;
	}

	static void InvokeNoArgs(das::Context* ctx, das::SimFunction* fn, SceneContext& activeCtx)
	{
		if (!fn || !ctx)
		{
			return;
		}
		g_activeContext = &activeCtx;
		ctx->evalWithCatch(fn, nullptr, nullptr);
		g_activeContext = nullptr;

		if (const char* ex = ctx->getException())
		{
			ERROR(LogCategory::App, "daScript exception: {}", ex);
		}
	}

	void ScriptingSubsystem::CallOnAttach(ScriptHandle& handle, SceneContext& ctx)
	{
		InvokeNoArgs(handle.ctx, handle.onAttach, ctx);
	}

	void ScriptingSubsystem::CallOnUpdate(ScriptHandle& handle, SceneContext& ctx)
	{
		InvokeNoArgs(handle.ctx, handle.onUpdate, ctx);
	}

	void ScriptingSubsystem::CallOnDetach(ScriptHandle& handle, SceneContext& ctx)
	{
		InvokeNoArgs(handle.ctx, handle.onDetach, ctx);
	}
} // namespace aether::app::scripting
