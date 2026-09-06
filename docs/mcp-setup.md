# MCP setup

Install steps for the MCP servers this repo's agents expect. If an agent reports a missing server, install it from here.

> **Paths use `<user>` as a placeholder for your Windows username.** Substitute your own — e.g. `alexm` on the home PC, `alex.mollard` on the work PC. In PowerShell you can also use `$env:USERPROFILE` (or `%USERPROFILE%` inside `.cmd` files) to avoid hand-editing per machine.

| Server | Install command |
|--------|----------------|
| **Mind** | `git clone https://github.com/GabrielMartinMoran/mind.git C:\Users\<user>\mind && cd C:\Users\<user>\mind && pip install -e . && mind setup codex && mind setup opencode` |
| **AetherCore MCP** | Built into this repository. Run `./scripts/Install-Mcp.ps1` to register it with Codex, OpenCode, and Claude Code. See [AetherCore MCP](#aethercore-mcp) below. |
| **clangd-mcp** | `git clone https://github.com/felipeerias/clangd-mcp-server.git C:\Users\<user>\clangd-mcp-server && cd C:\Users\<user>\clangd-mcp-server && npm install && npx tsc` |
| **clangd-mcp launcher** | Create `C:\Users\<user>\.bun\bin\clangd-mcp.cmd`: `node "C:\Users\<user>\clangd-mcp-server\build\index.js"` |
| **Token Optimizer** | `npm install -g @cocaxcode/token-optimizer-mcp` |
| **Context7** | Used via `npx @upstash/context7-mcp` |
| **GitHub MCP** | Download `github-mcp-server_Windows_x86_64.zip` from releases, extract to `C:\Users\<user>\.bun\bin\github-mcp-server.exe`. Set `GITHUB_PERSONAL_ACCESS_TOKEN` env var. |
| **codebase-memory** | Download `codebase-memory-mcp-windows-amd64.zip` from releases, extract to `$env:LOCALAPPDATA\Programs\codebase-memory-mcp\codebase-memory-mcp.exe`. Run `codebase-memory-mcp install -y`. If Opencode wasn't auto-detected, add manually to `~\.config\opencode\opencode.jsonc`. |
| **graphify** | CLI that builds the `graphify-out/` knowledge graph. Surfaced to OpenCode via `.opencode/plugins/graphify.js` and to Claude Code as the `/graphify` skill. Rebuild with `graphify update .`; query with `graphify query "<question>"`. |
| **vs-mcp** | See [VisualStudio-MCP.md](VisualStudio-MCP.md) for setup. |

## AetherCore MCP

This repository ships its own stdlib-only MCP server at
`tools/mcp/aethercore_mcp.py`. It can run the build-and-test gauntlet without a
running editor, and can control a live editor through the local `aether-ctl`
bridge: scene and entity operations, editor panels, render-graph inspection,
GPU texture captures, screenshots, and play mode.

Build the editor and bridge, then register the server with the coding tools you
use:

```powershell
cmake --build --preset default --target Editor aether-ctl
./scripts/Install-Mcp.ps1
# Or limit registration, for example: ./scripts/Install-Mcp.ps1 -Targets codex
```

For live tools, start the Launcher: its small hub endpoint binds to `127.0.0.1`
on `8787` by default and exposes hub status, recent projects, and screenshots.
When you open a project, the Launcher hands that same port to the full Editor
endpoint automatically. `GameRuntime` never includes either endpoint.

```powershell
.\build\default\src\app\RelWithDebInfo\Launcher.exe
```

Alternatively, start a project-scoped Editor directly with a matching port:
`$env:AETHER_CONTROL_PORT = "8787"; .\build\default\src\app\RelWithDebInfo\Editor.exe --project <path>`.
You can also use the editor's **Control Server** panel to start or stop its
endpoint and choose its port. `run_gauntlet` needs no live app. The complete
tool catalog, manual registration examples, and environment variables are in
[tools/mcp/README.md](../tools/mcp/README.md).

## Synthetic input reachability from an agent session

A prior version of this section claimed real window focus is required for
"in-game keystroke/hold-state gameplay... interact prompts, WASD movement" and
similar. That was too broad and is corrected here with the precise matrix,
each row read from source rather than assumed - most gameplay input turns out
to be reachable without focus at all; only cursor lock itself is not.

**Reachable without real OS focus** - `engine.send_input`'s `down`/`up` keys
and `mouse_pos`/`mouse_down`/`mouse_up` write straight into
`Input::m_syntheticKeys`/`m_syntheticMouseButtons`/`m_syntheticMousePos`
(`Input.hpp:396-397`, `:456-469`). Both are OR'd into the real per-frame state
with no focus check at all - `Input.cpp:152`:
`m_currKeys[i] = (focused && glfwGetKey(...)) || m_syntheticKeys[i]`, and
`Input.cpp:159`: `m_currMouseButtons[i] = (focused && glfwGetMouseButton(...))
|| m_syntheticMouseButtons[i]` - the synthetic half sits outside the
`focused &&` term in both expressions. That is the exact same `IsKeyDown`/
`IsKeyPressed`/`IsMouseButtonDown`/`IsMouseDown`/`GetMousePos` state gameplay
scripts, `UiNavigationSystem`, and every `Ui.IsHovered`/`WasClicked`/
`WasActivated` hit-test read from - so WASD movement, interact prompts, menu
navigation, and clicking a game-rendered `UIButton` by its `ui.layout`
resolved-rect centre are all genuinely testable headlessly. Confirmed live,
not just read: `engine.send_input` `mouse_pos`→`mouse_down`→`mouse_up` hit
Sandbox's Host/Join/codeBox buttons on the first try across two separate
Editor builds.

**Not reachable without real OS focus** - cursor lock itself and its Escape
release, because `UpdateCursorLock` (`Input.cpp:509-527`) explicitly ANDs
`windowFocused` into `SetCursorLocked(m_cursorLockRequested && windowFocused
&& !m_cursorLockEscaped)`. A synthetic session can request the lock, but it
will never actually engage, so `Input.MouseDelta`'s locked-relative-motion
mode and anything gated on `IsCursorLocked()` cannot be exercised this way.
`computer`'s `delivery: "foreground"` mode exists to force real focus for
exactly this case, but fails outright (`SetForegroundWindow` error) whenever
the desktop session is locked or otherwise not the active foreground
session - the normal state for an unattended agent run.

**Never reaches game UI, focus irrelevant** - `ui.click`'s raw `{x,y}` mode
(and `ui.hover`/`ui.drag`). `UiInputScript::QueueClick` (`UiInputScript.cpp:61`),
drained in `UiAutomation.cpp:112-116`, injects via `ImGuiIO::AddMousePosEvent`/
`AddMouseButtonEvent` - these populate only ImGui's own `io.MousePos`/
`io.MouseDown[]`, read exclusively by ImGui widget hit-testing (the editor's
own panels; `UiAutomationMethods.cpp` lives under `src/app/imgui/`). There is
no wiring from ImGui's `io` to the engine's `Input` class at all, so a
`ui.click` at a game UI button's exact screen coordinate does nothing, on any
machine, focused or not - it is architecturally scoped to ImGui, not a focus
limitation. Use `ui.click`/`ui.query` for editor panels and dialogs (File
Explorer, "Recover Unsaved Work", Inspector); use `engine.send_input`'s mouse
path for anything the game itself renders.

**Implication:** verify cursor-lock-dependent behavior by asserting on
resulting state through the control protocol, or hand that one check to a
human with a real, unlocked, focused session - do not conclude "cursor lock
works" from a synthetic session that never had focus, since the lock silently
never engages. Everything else - keyboard gameplay, menu navigation, and
clicking game-rendered UI - is fair game for `engine.send_input` headlessly.

## Debugging a native crash reached through C# script

AGENTS.md's crash-section promise (minidump + stacks + log tail under
`%LOCALAPPDATA%/AetherCore/crashes/<exe>/`) **does not hold for crashes raised
through C# interop**, and this section is the correction plus the technique.
`physics-engine` established it live on 2026-09-06 while root-causing the
`Door.OnAttach` segfault (a hard 4/4-reproducible access violation produced
nothing in the crashes folder).

**Why nothing is written.** An access violation raised inside a P/Invoke'd
export fail-fasts through CoreCLR's own vectored exception handler, which calls
`TerminateProcess` before the engine's `SetUnhandledExceptionFilter`
(`CrashHandler.cpp:995`) is ever consulted. No engine dump, no all-thread
stacks, no log tail. The process just dies with exit code 2147483647 and
(sometimes) a CoreCLR "Fatal error. 0xC0000005" block on the console with a
managed-only stack naming the script frame - which is itself the best first
clue; copy it before it scrolls.

**The knob that does work** is CoreCLR's own minidump support, set in the
environment *before* the Editor launches (CoreCLR reads it at startup, not at
crash time):

```powershell
$env:COMPlus_DbgEnableMiniDump = "1"
$env:COMPlus_DbgMiniDumpName = "C:\temp\editor_crash.dmp"
.\build\default\src\app\RelWithDebInfo\Editor.exe --project projects\Sandbox
```

Expect a large file (the Door crash produced ~190 MB) - point the name at a
scratch location, not the repo.

**The debugging technique that cracked it**, and generalises to every
native-crash-reached-through-script: a debugger will not stop on the access
violation (same vectored-handler reason), so set a breakpoint on
**`TerminateProcess`** instead. When it hits, the crashing thread's stack is
still live and shows the whole native chain. For the Door crash that was
`Door.OnAttach` -> `Physics.SetMotionType` -> `aether_physics_set_motion_type`
-> `PhysicsSystem::SetBodyMotionType` -> `JPH::BodyInterface::SetMotionType` ->
`BodyManager::ActivateBodies` on a null `MotionProperties` - i.e. the script
called into physics before the Jolt body existed.

**Root cause and fix** for that specific crash: committed as `f110d153`
(`PhysicsSystem.cpp` refuses/queues the motion-type change for a body that has
not been baked yet, rather than dereferencing it).
