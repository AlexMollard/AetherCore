#include "ScriptingSubsystem.hpp"
#include "ScriptHandle.hpp"
#include "SceneContext.hpp"
#include "DasModuleBase.hpp" // GetModuleRegistrars()
#include "VfsFileAccess.hpp"

// Pull in the full daScript API after our own headers (heavy include).
#include "daScript/daScript.h"

#include "scene/World.hpp"
#include "utils/Logger.hpp"

#include <filesystem>

// ── Module registration ────────────────────────────────────────────────────────
// Modules self-register via AETHER_DAS_MODULE in their own .cpp files.
// Adding a new module requires zero changes here.

// WorldModule defines the World type annotation; it must be registered before
// any other module whose addExtern calls reference aether::World*.
DECLARE_MODULE(WorldModule);

static void EnsureModulesRegistered()
{
	NEED_ALL_DEFAULT_MODULES;
	PULL_MODULE(WorldModule); // first - others depend on World type annotation
	for (auto reg: GetModuleRegistrars())
	{
		reg();
	}
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

		// Ensure daslang knows where its source tree (daslib) is so requires
		// like `require daslib/json` can be resolved. Try common locations
		// relative to the working directory (build output) and set the
		// das root if we find a daslib directory.
		try {
			using fs = std::filesystem::path;
			fs cwd = std::filesystem::current_path();
			std::vector<fs> candidates = {
				cwd / "_deps" / "dascript-src",
				cwd / "build" / "_deps" / "dascript-src",
				cwd / ".." / "_deps" / "dascript-src",
				cwd / ".." / "build" / "_deps" / "dascript-src",
				cwd / "_deps" / "dascript-src",
			};
			for (const auto &c : candidates) {
				std::error_code ec;
                if (!c.empty() && std::filesystem::exists(c / "daslib", ec)) {
                    das::setDasRoot(c.string());
                    AE_INFO(LogCategory::App, "daslang root set to '{}'.", c.string());
                    break;
                }
			}
		} catch (const std::exception &e) {
			AE_WARN(LogCategory::App, "Failed to auto-detect daslang root: {}", e.what());
		}

		AE_INFO(LogCategory::App, "ScriptingSubsystem initialised.");
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
	static std::string ResolveScriptPath(const std::string& path)
	{
		const std::filesystem::path rel(path);
		if (rel.is_absolute())
		{
			return path;
		}

		const auto cwd = std::filesystem::current_path();

		std::vector<std::filesystem::path> roots;
#ifdef AETHER_SCRIPTS_SOURCE_DIR
		roots.emplace_back(AETHER_SCRIPTS_SOURCE_DIR);
#endif
		roots.emplace_back(cwd / "data" / "scripts");
		roots.emplace_back(cwd / "../data/scripts");
		roots.emplace_back(cwd / "../../data/scripts");
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
		return path;
	}

	ScriptHandle ScriptingSubsystem::Compile(const std::string& path)
	{
		m_lastError.clear();
		const std::string resolvedPath = ResolveScriptPath(path);

		auto fAccess = das::make_smart<VfsFileAccess>();

		const std::string scriptsRoot = "scripts://";
		fAccess->AddSearchRoot("scripts", scriptsRoot + "scripts");

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
			AE_ERROR(LogCategory::App, "ScriptingSubsystem: {}", m_lastError);
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
			AE_ERROR(LogCategory::App, "ScriptingSubsystem: {}", m_lastError);
			delete ctx;
			return {};
		}

		ScriptHandle handle;
		handle.scriptPath = resolvedPath;
		handle.ctx = ctx;
		handle.onAttach = ctx->findFunction("on_attach");
		handle.onUpdate = ctx->findFunction("on_update");
		handle.onDetach = ctx->findFunction("on_detach");

		AE_INFO(LogCategory::App, "Script compiled: '{}' (attach={} update={} detach={})", resolvedPath, handle.onAttach != nullptr, handle.onUpdate != nullptr, handle.onDetach != nullptr);

		return handle;
	}

	// Invoke a script function with World as the first argument.
	static void InvokeWithWorld(das::Context* ctx, das::SimFunction* fn, SceneContext& activeCtx)
	{
		if (!fn || !ctx)
		{
			return;
		}

		g_activeContext = &activeCtx;

		// Pass World* as the first (and only) script argument.
		// Scripts declare: def on_attach(world : World) { ... }
		vec4f worldArg = das::cast<aether::World*>::from(activeCtx.world);
		ctx->evalWithCatch(fn, &worldArg, nullptr);

		g_activeContext = nullptr;

		if (const char* ex = ctx->getException())
		{
			AE_ERROR(LogCategory::App, "daScript exception: {}", ex);
		}
	}

	void ScriptingSubsystem::CallOnAttach(ScriptHandle& handle, SceneContext& ctx)
	{
		InvokeWithWorld(handle.ctx, handle.onAttach, ctx);
	}

	void ScriptingSubsystem::CallOnUpdate(ScriptHandle& handle, SceneContext& ctx)
	{
		InvokeWithWorld(handle.ctx, handle.onUpdate, ctx);
	}

	void ScriptingSubsystem::CallOnDetach(ScriptHandle& handle, SceneContext& ctx)
	{
		InvokeWithWorld(handle.ctx, handle.onDetach, ctx);
	}
} // namespace aether::app::scripting
