# INKBOUND Dialogue System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A branching, data-driven, on-theme dialogue system for INKBOUND — JSON conversations with choices, three trigger paths, animated ink-styled presentation with per-glyph rich-text effects and a portrait slot.

**Architecture:** All game logic is INKBOUND project code (`projects/INKBOUND/scripts/` + `assets/dialogue/*.json` + one project shader). The single engine change is a *generic* asset-text read (`Assets.ReadText`). A persistent `DialogueRunner` script builds the box UI at runtime, freezes gameplay via `Time.Pause`, and animates on `Time.UnscaledTime`. Pure `DialogueGraph`/`DialogueState` classes hold parsing + branching logic.

**Tech Stack:** C# (CoreCLR scripts, `System.Text.Json`), C++20 interop (`AE_SCRIPT_API`), Slang UI material shader, the existing `Ui.*` / `Physics2D` / `Time` / `Input` SDK APIs.

**Design spec:** `docs/superpowers/specs/2026-07-22-inkbound-dialogue-system-design.md` (read it — this plan implements it).

## Global Constraints

- **Project boundary (hard rule):** NO dialogue-specific code in `src/engine`, `src/app`, or `managed/AetherCore` (SDK). The ONLY engine/SDK change is the generic `Assets.ReadText` (Task 1). Everything else lives under `projects/INKBOUND/`.
- **No shortcuts** — proper architecture, not timer/dirty-flag hacks.
- **ASCII-only** UI text (font atlas limitation).
- **Text is a project asset**, addressed via the VFS `project://` mount (exact prefix pinned in Task 1).
- **New interop `.cpp` requires a CMake reconfigure** (`cmake -S . -B build/<dir>`) or the Editor target omits it → `EntryPointNotFoundException`.
- **Managed csprojs are SDK-style** (glob `**/*.cs`), so new `.cs` files need no csproj edit.
- **Shaders bind `g_textures[]` at set 0** (bindless rule); project shaders compile on Play and can `#include` engine shader headers.
- **Verification is in-editor via the aethercore MCP** (`play`, `send_input`/`ui_key`, `screenshot`, `get_console_log`, `stop`). Mouse cannot be injected headlessly — dialogue advance + choice nav are keyboard-drivable, so drive with keys. **NEVER** use desktop/computer control.
- **Finish rule:** at the end, fast-forward merge + push to `master`.
- Address the user as "Pog Champ".

## Reference patterns (verified in the codebase)

- **String-return interop** (`UiExports.cpp:46` `aether_ui_get_text`): `AE_SCRIPT_API std::int32_t f(..., char* buf, std::int32_t bufLen)` → `memcpy`, return byte count.
- **Native.cs decl:** `[LibraryImport(Lib)] internal static partial int f(uint id, byte* buf, int bufLen);` and for string args `[LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]`.
- **Managed two-call read** (`Ui.GetText`): `stackalloc byte[N]`, `fixed`, `Encoding.UTF8.GetString`.
- **EntityScript:** `Self`, `OnAttach/OnUpdate/OnDetach`, `OnTriggerEnter2D(Entity)`, `StartCoroutine(IEnumerator)`, `WaitForSeconds`.
- **Time:** `Time.Pause()`, `Time.Resume()`, `Time.UnscaledTime`, `Time.IsPaused`.
- **Input:** `Input.IsKeyPressed(Key)`, `Input.IsMousePressed(MouseButton.Left)`, `Input.MousePosition`.
- **Ui:** `CreateImage/CreateText`, `SetAnchors/SetPivot/SetRect`, `SetText/SetFontSize/SetTextColor/SetTextAlign`, `SetImageColor/SetImageCornerRadius/SetImageTexture`, `GetRect`, `IsHovered/WasClicked`, `SetMaterial/SetMaterialParams/SetMaterialColors`. `Ui.CreateText(canvas, text)` — pass `default` canvas to attach to the first/created canvas.
- **Persistent script:** `Self.DontDestroyOnLoad()` (see `HudController`).
- **Runtime spawn:** `Scene.Create(name, pos)`, `Scene.Find(name)`, `Scene.Instantiate(prefab, pos)`.
- **Palette:** `GameSettings.Accent = (0.302, 0.851, 1.0, 1)`; ink near-black `(0.02,0.03,0.05)`; off-white text `(0.93,0.95,0.97)`. Fonts: body `IBMPlexMono-Italic`, display `PixelStorm`.
- **Material shader template:** `projects/INKBOUND/assets/shaders/ui_glitch_text.slang` (binds `ui_shapes` vertex, masks to glyph SDF via `type == kShapeSdfGlyph`, uses `texUV`/`textureSlot`/`position`).

---

## Task 1: Generic asset-text read (`Assets.ReadText`)

**Files:**
- Create: `src/app/scripting/interop/AssetsExports.cpp`
- Modify: `managed/AetherCore/Internal/Native.cs` (add one P/Invoke)
- Create: `managed/AetherCore/Assets.cs`
- Temp probe: `projects/INKBOUND/scripts/DialogueAssetProbe.cs` (added then removed)
- Temp data: `projects/INKBOUND/assets/dialogue/_probe.txt` (added then removed)

**Interfaces:**
- Produces: `AetherCore.Assets.ReadText(string virtualPath) -> string?` (null if missing). Used by `Dialogue.Play` (Task 4).

- [ ] **Step 1: Write the interop.** Create `src/app/scripting/interop/AssetsExports.cpp`:

```cpp
// Generic managed asset-text read. Runtime-safe: engine io only, no editor/ImGui deps.
// Reads a VFS virtual path as UTF-8 text (raw project dir in-editor, pak when shipped).

#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>

#include "io/FileSystem.hpp"

// Fills `out` with up to `cap` bytes of the asset text. Returns the FULL byte length
// (may exceed cap -> caller grows and retries). Returns -1 if the asset is missing/unreadable.
AE_SCRIPT_API std::int32_t aether_assets_read_text(const char* vpath, char* out, std::int32_t cap)
{
	if (vpath == nullptr)
	{
		return -1;
	}
	auto text = aether::io::FileSystem::ReadFileText(vpath);
	if (!text)
	{
		return -1;
	}
	const std::int32_t full = static_cast<std::int32_t>(text->size());
	if (out != nullptr && cap > 0)
	{
		const std::int32_t n = std::min<std::int32_t>(cap, full);
		std::memcpy(out, text->data(), static_cast<std::size_t>(n));
	}
	return full;
}
```

- [ ] **Step 2: Add the P/Invoke.** In `managed/AetherCore/Internal/Native.cs`, near the other string-buffer imports (e.g. after `aether_ui_get_text`), add:

```csharp
[LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
internal static partial int aether_assets_read_text(string vpath, byte* buf, int cap);
```

- [ ] **Step 3: Add the managed API.** Create `managed/AetherCore/Assets.cs`:

```csharp
using System;
using System.Text;

namespace AetherCore;

/// <summary>Generic data-asset access. Reads through the VFS: the raw project directory
/// in-editor, the packed .pak when shipped. Not tied to any game.</summary>
public static class Assets
{
    /// <summary>Read a data asset as UTF-8 text (e.g. "project://dialogue/intro.json").
    /// Returns null if the asset does not exist.</summary>
    public static unsafe string? ReadText(string virtualPath)
    {
        // First call sizes the asset, second fills. Grow once if the stack buffer is too small.
        Span<byte> buffer = stackalloc byte[4096];
        fixed (byte* ptr = buffer)
        {
            int full = Native.aether_assets_read_text(virtualPath, ptr, buffer.Length);
            if (full < 0)
            {
                return null;
            }
            if (full <= buffer.Length)
            {
                return Encoding.UTF8.GetString(ptr, full);
            }
        }
        byte[] heap = new byte[/* full */ 0]; // replaced below
        return ReadLarge(virtualPath);
    }

    private static unsafe string ReadLarge(string virtualPath)
    {
        int size;
        fixed (byte* probe = new byte[1])
        {
            size = Native.aether_assets_read_text(virtualPath, probe, 0); // cap 0 -> just the length
        }
        byte[] heap = new byte[size];
        fixed (byte* ptr = heap)
        {
            int full = Native.aether_assets_read_text(virtualPath, ptr, heap.Length);
            return full > 0 ? Encoding.UTF8.GetString(ptr, Math.Min(full, heap.Length)) : string.Empty;
        }
    }
}
```

*(Note: the interop returns the full length even when `cap` is smaller, so `ReadLarge` re-reads with an exact buffer. Dialogue files are a few KB, so the 4096 fast path almost always hits.)*

- [ ] **Step 4: Reconfigure CMake so the Editor target compiles the new TU.**

Run:
```bash
cmake -S D:/AetherCore -B D:/AetherCore/build/vs2022-msvc
```
Expected: configures without error, picks up `AssetsExports.cpp`.

- [ ] **Step 5: Build the Editor + managed assemblies.**

Run:
```bash
cmake --build D:/AetherCore/build/vs2022-msvc --target Editor App_CompileShaders --config Debug
```
Expected: builds; `aether_assets_read_text` is exported.

- [ ] **Step 6: Write the probe to pin the working VFS prefix.** Create `projects/INKBOUND/assets/dialogue/_probe.txt` containing `hello-ink`. Create `projects/INKBOUND/scripts/DialogueAssetProbe.cs`:

```csharp
using AetherCore;
namespace AetherGame;
// TEMPORARY — pins the VFS prefix for dialogue assets, then gets deleted (Task 1 only).
public sealed class DialogueAssetProbe : EntityScript
{
    public override void OnAttach()
    {
        foreach (string p in new[] { "project://dialogue/_probe.txt", "project://assets/dialogue/_probe.txt" })
        {
            string? t = Assets.ReadText(p);
            Log.Info($"[PROBE] {p} => {(t == null ? "NULL" : t.Trim())}");
        }
    }
}
```

- [ ] **Step 7: Verify in-editor which prefix resolves.** Attach `DialogueAssetProbe` to any entity in `Menu` (or Level1), Play, read the console. Via MCP: `add_script` the probe to an entity, `play`, `get_console_log`, `stop`.

Expected: exactly one `[PROBE]` line logs `hello-ink` (the other logs `NULL`). **Record the working prefix** — call it `DlgRoot` (e.g. `project://dialogue/` or `project://assets/dialogue/`). Every later task uses `DlgRoot + "<id>.json"`.

- [ ] **Step 8: Remove the probe.** Delete `DialogueAssetProbe.cs` and `_probe.txt`:
```bash
rm D:/AetherCore/projects/INKBOUND/scripts/DialogueAssetProbe.cs D:/AetherCore/projects/INKBOUND/assets/dialogue/_probe.txt
```

- [ ] **Step 9: Commit.**
```bash
git add -A && git commit -m "Add generic Assets.ReadText VFS text read"
```

---

## Task 2: Dialogue data model + JSON parse + run tokenizer (pure C#)

**Files:**
- Create: `projects/INKBOUND/scripts/DialogueMarkup.cs` (inline `[tag]` tokenizer)
- Create: `projects/INKBOUND/scripts/DialogueGraph.cs` (model + JSON parse)
- Test: `projects/INKBOUND/scripts/DialogueSelfTest.cs` (temporary in-editor asserts; removed in Task 3)

**Interfaces:**
- Produces:
  - `enum InkEffect { Normal, Shake, Wave, Flicker, Whisper, Glitch }`
  - `record TextRun(string Text, InkEffect Effect)`
  - `DialogueMarkup.Tokenize(string raw, InkEffect nodeDefault) -> (string plain, List<TextRun> runs)`
  - `class DialogueNode { string Speaker; string? Portrait; string Text; InkEffect Effect; List<TextRun> Runs; string? Goto; List<DialogueChoice> Choices; }`
  - `class DialogueChoice { string Text; string Goto; string? If; string? Set; }`
  - `class DialogueGraph { string Id; string Start; Dictionary<string,DialogueNode> Nodes; static DialogueGraph? Parse(string json); }`

- [ ] **Step 1: Write the markup tokenizer.** Create `projects/INKBOUND/scripts/DialogueMarkup.cs`:

```csharp
using System.Collections.Generic;
using System.Text;

namespace AetherGame;

/// <summary>Per-glyph rich-text effect selectable per run. Ids match ui_dialogue_text.slang.</summary>
public enum InkEffect { Normal = 0, Shake = 1, Wave = 2, Flicker = 3, Whisper = 4, Glitch = 5 }

/// <summary>One contiguous run of body text sharing a single effect.</summary>
public readonly record struct TextRun(string Text, InkEffect Effect);

/// <summary>Splits inline "[effect]...[/effect]" markup into styled runs. Untagged text takes the
/// node-level default effect. Returns the tag-free plain string (for width/typewriter math) and the
/// run list. Unknown tags are treated as literal text (kept, no effect) so authoring typos are visible.</summary>
public static class DialogueMarkup
{
    public static (string Plain, List<TextRun> Runs) Tokenize(string raw, InkEffect nodeDefault)
    {
        var runs = new List<TextRun>();
        var plain = new StringBuilder();
        var cur = new StringBuilder();
        InkEffect curEffect = nodeDefault;

        void Flush()
        {
            if (cur.Length > 0) { runs.Add(new TextRun(cur.ToString(), curEffect)); cur.Clear(); }
        }

        int i = 0;
        while (i < raw.Length)
        {
            if (raw[i] == '[')
            {
                int close = raw.IndexOf(']', i + 1);
                if (close > i)
                {
                    string tag = raw.Substring(i + 1, close - i - 1);
                    if (tag.StartsWith("/") && TryEffect(tag.Substring(1), out _))
                    {
                        Flush(); curEffect = nodeDefault; i = close + 1; continue;
                    }
                    if (TryEffect(tag, out InkEffect eff))
                    {
                        Flush(); curEffect = eff; i = close + 1; continue;
                    }
                }
            }
            cur.Append(raw[i]);
            plain.Append(raw[i]);
            i++;
        }
        Flush();
        if (runs.Count == 0) { runs.Add(new TextRun(string.Empty, nodeDefault)); }
        return (plain.ToString(), runs);
    }

    public static bool TryEffect(string name, out InkEffect effect)
    {
        switch (name)
        {
            case "normal":  effect = InkEffect.Normal;  return true;
            case "shake":   effect = InkEffect.Shake;   return true;
            case "wave":    effect = InkEffect.Wave;    return true;
            case "flicker": effect = InkEffect.Flicker; return true;
            case "whisper": effect = InkEffect.Whisper; return true;
            case "glitch":  effect = InkEffect.Glitch;  return true;
            default:        effect = InkEffect.Normal;  return false;
        }
    }
}
```

- [ ] **Step 2: Write the graph model + parser.** Create `projects/INKBOUND/scripts/DialogueGraph.cs`:

```csharp
using System.Collections.Generic;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

public sealed class DialogueChoice
{
    public string Text = "";
    public string Goto = "";
    public string? If;
    public string? Set;
}

public sealed class DialogueNode
{
    public string Speaker = "";
    public string? Portrait;
    public string Text = "";
    public InkEffect Effect = InkEffect.Normal;
    public List<TextRun> Runs = new();
    public string? Goto;
    public List<DialogueChoice> Choices = new();
    public bool HasChoices => Choices.Count > 0;
}

/// <summary>A parsed conversation. Pure data — no engine calls. Built by <see cref="Parse"/> from the
/// JSON schema in the design spec (§2). Returns null on malformed JSON (logged, never throws to caller).</summary>
public sealed class DialogueGraph
{
    public string Id = "";
    public string Start = "";
    public Dictionary<string, DialogueNode> Nodes = new();

    public DialogueNode? NodeOrNull(string? id) =>
        id != null && Nodes.TryGetValue(id, out var n) ? n : null;

    public static DialogueGraph? Parse(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;
            var g = new DialogueGraph
            {
                Id = GetStr(root, "id") ?? "",
                Start = GetStr(root, "start") ?? "",
            };
            if (!root.TryGetProperty("nodes", out JsonElement nodes) || nodes.ValueKind != JsonValueKind.Object)
            {
                Log.Warn("[INKBOUND] dialogue: missing 'nodes' object");
                return null;
            }
            foreach (JsonProperty np in nodes.EnumerateObject())
            {
                JsonElement ne = np.Value;
                var node = new DialogueNode
                {
                    Speaker = GetStr(ne, "speaker") ?? "",
                    Portrait = GetStr(ne, "portrait"),
                    Text = GetStr(ne, "text") ?? "",
                    Goto = GetStr(ne, "goto"),
                };
                if (DialogueMarkup.TryEffect(GetStr(ne, "effect") ?? "normal", out InkEffect eff)) { node.Effect = eff; }
                (_, node.Runs) = DialogueMarkup.Tokenize(node.Text, node.Effect);
                node.Text = StripTags(node.Runs); // plain, tag-free
                if (ne.TryGetProperty("choices", out JsonElement ch) && ch.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement ce in ch.EnumerateArray())
                    {
                        node.Choices.Add(new DialogueChoice
                        {
                            Text = GetStr(ce, "text") ?? "",
                            Goto = GetStr(ce, "goto") ?? "",
                            If = GetStr(ce, "if"),
                            Set = GetStr(ce, "set"),
                        });
                    }
                }
                g.Nodes[np.Name] = node;
            }
            if (g.Start.Length == 0 || !g.Nodes.ContainsKey(g.Start))
            {
                Log.Warn($"[INKBOUND] dialogue '{g.Id}': start node '{g.Start}' missing");
                return null;
            }
            return g;
        }
        catch (JsonException e)
        {
            Log.Warn($"[INKBOUND] dialogue parse failed: {e.Message}");
            return null;
        }
    }

    private static string StripTags(List<TextRun> runs)
    {
        var sb = new System.Text.StringBuilder();
        foreach (TextRun r in runs) { sb.Append(r.Text); }
        return sb.ToString();
    }

    private static string? GetStr(JsonElement e, string name) =>
        e.TryGetProperty(name, out JsonElement v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
}
```

- [ ] **Step 3: Write the self-test (fails first).** Create `projects/INKBOUND/scripts/DialogueSelfTest.cs`:

```csharp
using System.Collections.Generic;
using AetherCore;

namespace AetherGame;

// TEMPORARY in-editor test harness (no C# unit runner exists). Attach to any entity, Play,
// read the console for [DLGTEST] PASS/FAIL lines, then remove after Task 3.
public sealed class DialogueSelfTest : EntityScript
{
    private int _pass, _fail;
    private void Check(bool cond, string label)
    {
        if (cond) { _pass++; Log.Info($"[DLGTEST] PASS {label}"); }
        else      { _fail++; Log.Warn($"[DLGTEST] FAIL {label}"); }
    }

    public override void OnAttach()
    {
        const string json = @"{ ""id"":""t"", ""start"":""a"", ""nodes"": {
            ""a"": { ""speaker"":""V"", ""text"":""go [shake]down[/shake] now"", ""effect"":""whisper"",
                     ""choices"":[ {""text"":""x"",""goto"":""b"",""set"":""f""},
                                   {""text"":""y"",""goto"":""c"",""if"":""coins>=3""} ] },
            ""b"": { ""speaker"":""V"", ""text"":""deeper"", ""goto"":""end"" } } }";

        DialogueGraph? g = DialogueGraph.Parse(json);
        Check(g != null, "parse ok");
        Check(g!.Nodes.Count == 2, "node count 2");
        DialogueNode a = g.Nodes["a"];
        Check(a.Text == "go down now", "tags stripped from plain text");
        Check(a.Runs.Count == 3, "three runs (whisper|shake|whisper)");
        Check(a.Runs[1].Effect == InkEffect.Shake, "middle run is shake");
        Check(a.Runs[0].Effect == InkEffect.Whisper, "outer runs take node default whisper");
        Check(a.Choices.Count == 2, "two choices");
        Check(a.Choices[1].If == "coins>=3", "choice condition parsed");
        Check(DialogueGraph.Parse("{ not json") == null, "malformed -> null");

        // DialogueState checks are added in Task 3.
        Log.Info($"[DLGTEST] DONE pass={_pass} fail={_fail}");
    }
}
```

- [ ] **Step 4: Run it and watch it fail, then pass.** Build the game assembly (Play triggers a script recompile) or `cmake --build ... --target Editor`. Via MCP: attach `DialogueSelfTest` to an entity in `Menu`, `play`, `get_console_log`, `stop`.

Expected on completion of Steps 1-2: `[DLGTEST] DONE pass=9 fail=0`. If any FAIL, fix `DialogueGraph`/`DialogueMarkup` before continuing.

- [ ] **Step 5: Commit.**
```bash
git add projects/INKBOUND/scripts/DialogueMarkup.cs projects/INKBOUND/scripts/DialogueGraph.cs projects/INKBOUND/scripts/DialogueSelfTest.cs
git commit -m "Add dialogue graph model, JSON parse, and inline-effect tokenizer"
```

---

## Task 3: Dialogue state — flags, conditions, effects (pure C#)

**Files:**
- Create: `projects/INKBOUND/scripts/DialogueState.cs`
- Modify: `projects/INKBOUND/scripts/DialogueSelfTest.cs` (add condition asserts), then delete it at the end.

**Interfaces:**
- Produces: `static class DialogueState { void SetFlag(string); bool HasFlag(string); bool Evaluate(string? cond); void ResetRun(); }`
- Consumes: `GameState.TotalCoins` (for `coins>=N`).

- [ ] **Step 1: Write the state + evaluator.** Create `projects/INKBOUND/scripts/DialogueState.cs`:

```csharp
using System.Collections.Generic;
using AetherCore;

namespace AetherGame;

/// <summary>Global dialogue variables for a run. Flags persist across conversations (static, like
/// GameState) and are cleared on a fresh run. Conditions are the minimal grammar from the spec:
/// "flag" (set), "!flag" (unset), "coins>=N" (run coin count). Unknown -> false + warning.</summary>
public static class DialogueState
{
    private static readonly HashSet<string> s_flags = new();

    public static void SetFlag(string flag) { if (!string.IsNullOrEmpty(flag)) s_flags.Add(flag); }
    public static bool HasFlag(string flag) => s_flags.Contains(flag);
    public static void ResetRun() => s_flags.Clear();

    /// <summary>Evaluate a condition string. Null/empty => true (no gate).</summary>
    public static bool Evaluate(string? cond)
    {
        if (string.IsNullOrWhiteSpace(cond)) { return true; }
        cond = cond.Trim();

        if (cond.StartsWith("!")) { return !HasFlag(cond.Substring(1).Trim()); }

        int op = cond.IndexOf(">=");
        if (op > 0)
        {
            string lhs = cond.Substring(0, op).Trim();
            if (int.TryParse(cond.Substring(op + 2).Trim(), out int n))
            {
                if (lhs == "coins") { return GameState.TotalCoins >= n; }
            }
            Log.Warn($"[INKBOUND] dialogue: unknown condition '{cond}'");
            return false;
        }

        // Bare token => flag presence.
        if (IsFlagToken(cond)) { return HasFlag(cond); }
        Log.Warn($"[INKBOUND] dialogue: unknown condition '{cond}'");
        return false;
    }

    private static bool IsFlagToken(string s)
    {
        foreach (char c in s) { if (!(char.IsLetterOrDigit(c) || c == '_')) return false; }
        return s.Length > 0;
    }
}
```

- [ ] **Step 2: Extend the self-test.** In `DialogueSelfTest.cs`, before the `DONE` log, add:

```csharp
DialogueState.ResetRun();
Check(DialogueState.Evaluate(null), "null condition => true");
Check(!DialogueState.Evaluate("f"), "unset flag => false");
DialogueState.SetFlag("f");
Check(DialogueState.Evaluate("f"), "set flag => true");
Check(!DialogueState.Evaluate("!f"), "!setflag => false");
Check(DialogueState.Evaluate("!g"), "!unsetflag => true");
GameState.TotalCoins = 5;
Check(DialogueState.Evaluate("coins>=3"), "coins>=3 with 5 => true");
Check(!DialogueState.Evaluate("coins>=9"), "coins>=9 with 5 => false");
Check(!DialogueState.Evaluate("bogus>=1"), "unknown lhs => false");
DialogueState.ResetRun();
```

(Update the expected pass count accordingly: 9 + 8 = 17.)

- [ ] **Step 3: Run to green.** Play in-editor, read console. Expected: `[DLGTEST] DONE pass=17 fail=0`.

- [ ] **Step 4: Remove the temporary self-test.**
```bash
rm D:/AetherCore/projects/INKBOUND/scripts/DialogueSelfTest.cs
```

- [ ] **Step 5: Commit.**
```bash
git add -A && git commit -m "Add dialogue flag/condition state; drop temp self-test"
```

---

## Task 4: `DialogueRunner` — box UI, typewriter, linear playback, `Dialogue` facade

**Files:**
- Create: `projects/INKBOUND/scripts/Dialogue.cs` (static facade)
- Create: `projects/INKBOUND/scripts/DialogueRunner.cs` (the runtime + UI)
- Modify: `projects/INKBOUND/scripts/PauseController.cs` (guard: don't open pause menu while a dialogue is active)
- Modify: `projects/INKBOUND/scripts/PlayerController.cs` (guard: ignore gameplay input while a dialogue is active)
- Modify: one scene (e.g. add a `DialogueRunner` host) — see Step 6.

**Interfaces:**
- Produces:
  - `static class Dialogue { void Play(string id); bool IsActive; void Register(DialogueRunner); void SetFlag/HasFlag; }`
  - `DialogueRunner.Begin(DialogueGraph)`, `DialogueRunner.Advance()`
- Consumes: `Assets.ReadText` (Task 1, prefix `DlgRoot`), `DialogueGraph.Parse` (Task 2), `DialogueState` (Task 3).

- [ ] **Step 1: Write the facade.** Create `projects/INKBOUND/scripts/Dialogue.cs`:

```csharp
using AetherCore;

namespace AetherGame;

/// <summary>Entry point for starting conversations from anywhere (triggers, scripts). Loads the JSON
/// asset, parses it, and hands it to the single active DialogueRunner. Pins the VFS prefix found in
/// Task 1 (DlgRoot).</summary>
public static class Dialogue
{
    // Pinned by the Task 1 probe (project:// maps to the project ROOT, assets live under it):
    private const string DlgRoot = "project://assets/dialogue/";

    private static DialogueRunner? s_runner;

    public static bool IsActive => s_runner != null && s_runner.Active;

    internal static void Register(DialogueRunner r) => s_runner = r;
    internal static void Unregister(DialogueRunner r) { if (s_runner == r) s_runner = null; }

    public static void Play(string id)
    {
        if (s_runner == null) { Log.Warn("[INKBOUND] Dialogue.Play: no DialogueRunner in scene"); return; }
        if (s_runner.Active) { return; } // one conversation at a time
        string path = $"{DlgRoot}{id}.json";
        string? json = Assets.ReadText(path);
        if (json == null) { Log.Warn($"[INKBOUND] Dialogue.Play: '{path}' not found"); return; }
        DialogueGraph? g = DialogueGraph.Parse(json);
        if (g == null) { return; }
        s_runner.Begin(g);
    }

    public static void SetFlag(string flag) => DialogueState.SetFlag(flag);
    public static bool HasFlag(string flag) => DialogueState.HasFlag(flag);
}
```

- [ ] **Step 2: Write the runner (linear core).** Create `projects/INKBOUND/scripts/DialogueRunner.cs`. This step is the linear playback + box; choices come in Task 5, portrait in Task 6, rich-text in Tasks 7-8, animation polish in Task 9 (structure the class so those slot in):

```csharp
using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>The one persistent dialogue presenter. Builds its ink box UI at runtime, freezes gameplay
/// while a conversation runs (Time.Pause), and animates on Time.UnscaledTime. Attach to a host entity
/// in every gameplay scene (or a DontDestroyOnLoad host). Linear playback here; choices/portrait/
/// rich-text/animation added in later tasks.</summary>
public sealed class DialogueRunner : EntityScript
{
    // Palette (matches the menu theme).
    private static readonly Vector4 InkPanel = new(0.02f, 0.03f, 0.05f, 0.94f);
    private static readonly Vector4 Accent = GameSettings.Accent;
    private static readonly Vector4 BodyCol = new(0.93f, 0.95f, 0.97f, 1f);

    // Layout (screen px; box hugs the bottom third).
    private const float BoxH = 200f, Margin = 40f, Pad = 26f;
    private const float CharsPerSec = 42f;

    private Entity _canvas, _panel, _rule, _speaker, _body, _hint;
    private bool _built;

    public bool Active { get; private set; }
    private DialogueGraph? _graph;
    private DialogueNode? _node;
    private float _reveal;      // characters revealed so far (float, grows on UnscaledTime)
    private bool _fullShown;

    public override void OnAttach()
    {
        Self.DontDestroyOnLoad();
        Dialogue.Register(this);
        Build();
        Hide();
    }

    public override void OnDetach() => Dialogue.Unregister(this);

    private void Build()
    {
        if (_built) return;
        _canvas = Ui.CreateCanvas();

        _panel = Ui.CreateImage(_canvas);
        Ui.SetAnchors(_panel, new Vector2(0f, 1f), new Vector2(1f, 1f)); // top-anchored; we place by pixels from top
        Ui.SetPivot(_panel, new Vector2(0f, 0f));
        Ui.SetImageColor(_panel, InkPanel);
        Ui.SetImageCornerRadius(_panel, 8f);

        _rule = Ui.CreateImage(_canvas); // thin accent rule along the top edge
        Ui.SetAnchors(_rule, new Vector2(0f, 1f), new Vector2(1f, 1f));
        Ui.SetPivot(_rule, new Vector2(0f, 0f));
        Ui.SetImageColor(_rule, Accent);

        _speaker = Ui.CreateText(_canvas, "");
        Ui.SetFontSize(_speaker, 22f);
        Ui.SetTextColor(_speaker, Accent);
        Ui.SetTextAlign(_speaker, UiHAlign.Left, UiVAlign.Top);

        _body = Ui.CreateText(_canvas, "");
        Ui.SetFontSize(_body, 26f);
        Ui.SetTextColor(_body, BodyCol);
        Ui.SetTextAlign(_body, UiHAlign.Left, UiVAlign.Top);

        _hint = Ui.CreateText(_canvas, "> continue");
        Ui.SetFontSize(_hint, 16f);
        Ui.SetTextColor(_hint, new Vector4(Accent.X, Accent.Y, Accent.Z, 0.6f));
        Ui.SetTextAlign(_hint, UiHAlign.Right, UiVAlign.Bottom);

        _built = true;
        LayoutBox();
    }

    // Resolve pixel rects from the current screen size each time we show (handles resize).
    private void LayoutBox()
    {
        // Screen size via the panel's parent canvas is implicit; use a fixed reference and anchors.
        // Place the panel Margin from left/right, BoxH tall, sitting Margin above the bottom.
        // Anchored to top (anchor y=1), so y is measured downward from the top: use negative offset via SetRect.
        // Simpler: anchor the panel to the BOTTOM instead.
        Ui.SetAnchors(_panel, new Vector2(0f, 0f), new Vector2(1f, 0f));
        Ui.SetPivot(_panel, new Vector2(0f, 0f));
        Ui.SetOffsetsBox(_panel, Margin, Margin, Margin, BoxH); // helper below via SetRect fallback

        Ui.SetAnchors(_rule, new Vector2(0f, 0f), new Vector2(1f, 0f));
        Ui.SetPivot(_rule, new Vector2(0f, 0f));
    }

    public void Begin(DialogueGraph graph)
    {
        _graph = graph;
        Active = true;
        Time.Pause();
        Show();
        GoTo(graph.Start);
    }

    private void GoTo(string? nodeId)
    {
        _node = _graph?.NodeOrNull(nodeId);
        if (_node == null) { End(); return; }
        _reveal = 0f;
        _fullShown = false;
        Ui.SetText(_speaker, _node.Speaker);
        Ui.SetText(_body, "");
    }

    public override void OnUpdate(float dt)
    {
        if (!Active || _node == null) return;
        float u = Time.UnscaledTime;
        float udt = UnscaledDelta(u);

        // Typewriter.
        if (!_fullShown)
        {
            _reveal += udt * CharsPerSec;
            int shown = Math.Min(_node.Text.Length, (int)_reveal);
            Ui.SetText(_body, _node.Text.Substring(0, shown));
            if (shown >= _node.Text.Length) { _fullShown = true; }
        }

        bool advance = Input.IsKeyPressed(Key.Space) || Input.IsKeyPressed(Key.Enter)
                       || Input.IsMousePressed(MouseButton.Left) || Input.IsKeyPressed(Key.Escape);
        if (advance)
        {
            if (!_fullShown) { _reveal = _node.Text.Length; _fullShown = true; Ui.SetText(_body, _node.Text); }
            else { Next(); }
        }
    }

    private void Next()
    {
        // Linear only for now (choices in Task 5).
        GoTo(_node!.Goto);
    }

    private void End()
    {
        Active = false;
        _graph = null; _node = null;
        Hide();
        Time.Resume();
    }

    // ── helpers ──
    private float _lastU = -1f;
    private float UnscaledDelta(float u) { float d = _lastU < 0f ? 0f : Math.Max(0f, u - _lastU); _lastU = u; return d; }

    private void Show() { _panel.SetActive(true); _rule.SetActive(true); _speaker.SetActive(true); _body.SetActive(true); _hint.SetActive(true); }
    private void Hide() { if (!_built) return; _panel.SetActive(false); _rule.SetActive(false); _speaker.SetActive(false); _body.SetActive(false); _hint.SetActive(false); _lastU = -1f; }
}
```

> **Implementation note for the executor:** the exact anchor/offset math above is sketched — during Step 4 verify the box lands in the bottom third at the real screen size and adjust `LayoutBox` using `Ui.SetRect`/`SetAnchors`/`SetOffsets` (see `HudController` for the pixel-rect pattern). If `Ui.SetOffsetsBox` isn't a real API, compute an explicit `SetRect(_panel, Margin, Margin, screenW-2*Margin, BoxH)` using the resolved canvas width from `Ui.GetRect` of a full-screen probe, or anchor-stretch with `SetOffsets(min,max)`. Place `_rule` as a 3px-tall bar at the panel top, `_speaker` at `(Pad, Pad)`, `_body` at `(Pad, Pad+30)`, `_hint` bottom-right. Do **not** claim done until a screenshot shows the box correctly.

- [ ] **Step 3: Add the pause + player guards.**
  - In `PauseController.cs`, at the top of the input handler that opens the pause menu on Escape, early-return when `Dialogue.IsActive` (the dialogue consumes Escape itself).
  - In `PlayerController.cs`, at the top of its input/update where movement + ink drawing are read, early-return (skip gameplay input) when `Dialogue.IsActive`. (`Time.Pause` already freezes physics; this stops buffered jump/draw firing on the close frame.)

- [ ] **Step 4: Add a runner host + a linear test conversation.**
  - Create `projects/INKBOUND/assets/dialogue/_linear.json`:
    ```json
    { "id": "_linear", "start": "a", "nodes": {
        "a": { "speaker": "The Void", "text": "You should not have come this far down.", "goto": "b" },
        "b": { "speaker": "The Void", "text": "Then bring your ink. You will need it.", "goto": "end" } } }
    ```
  - Add a `DialogueRunner` script to an entity in `Level1` (via MCP `add_script` or scene edit). Temporarily call `Dialogue.Play("_linear")` — e.g. from a tiny temp trigger key: add to `DialogueRunner.OnUpdate` a dev line `if (!Active && Input.IsKeyPressed(Key.G)) Dialogue.Play("_linear");` (remove after this task).

- [ ] **Step 5: Verify in-editor.** Load `Level1`, `play`. Send `G` (`ui_key`/`send_input`), `screenshot`. Expected: box rises at the bottom, speaker "The Void", body types out char-by-char; pressing `Space`/`Enter` while typing snaps to full, again advances to line 2, again closes; the world is frozen while open (enemies/player don't move) and resumes after. Confirm `Time.IsPaused` behavior by observing a moving enemy halts. Remove the temp `G` line.

- [ ] **Step 6: Commit.**
```bash
git add -A && git commit -m "Add DialogueRunner box + typewriter + linear playback and Dialogue facade"
```

---

## Task 5: Choices — manual nav, conditions, effects, branching

**Files:**
- Modify: `projects/INKBOUND/scripts/DialogueRunner.cs`

**Interfaces:**
- Consumes: `DialogueState.Evaluate`/`SetFlag` (Task 3), the focus-marker style from `TitleScreen` (`▸` + cyan breathe).

- [ ] **Step 1: Add choice UI + state to the runner.** Add fields: `Entity[] _choiceUi = new Entity[6];`, `List<DialogueChoice> _visible = new();`, `int _focus;`. In `Build`, pre-create up to 6 choice text elements (`Ui.CreateText`), left-aligned, hidden. Add a `▸ ` prefix when focused.

- [ ] **Step 2: Show choices when the line finishes.** When `_fullShown` becomes true and `_node.HasChoices`, build `_visible` = choices whose `DialogueState.Evaluate(c.If)` is true; lay them under the body; set `_focus = 0`. While choices are visible, the advance key does **not** call `Next()`.

- [ ] **Step 3: Drive nav.** In `OnUpdate`, when choices are visible:
  - `Up`/`W` → `_focus = (_focus - 1 + n) % n`; `Down`/`S` → `+1`.
  - mouse hover: for each visible choice, `if (Ui.IsHovered(_choiceUi[i])) _focus = i;`
  - activate: `Input.IsKeyPressed(Key.Enter/Space)` or `Ui.WasClicked(_choiceUi[_focus])` → apply the choice.
  - Style: focused choice `▸ text` in cyan (breathe alpha like `TitleScreen`), others plain dim.

- [ ] **Step 4: Apply a choice.**
```csharp
private void Choose(int i)
{
    DialogueChoice c = _visible[i];
    if (!string.IsNullOrEmpty(c.Set)) { DialogueState.SetFlag(c.Set); }
    HideChoices();
    GoTo(c.Goto);
}
```
Update `Next()`/advance so a node with choices never linear-advances.

- [ ] **Step 5: Branch test.** Create `projects/INKBOUND/assets/dialogue/_branch.json`:
```json
{ "id":"_branch", "start":"a", "nodes": {
  "a": { "speaker":"The Void", "text":"Will you descend?",
         "choices":[ {"text":"Yes.","goto":"yes","set":"brave"},
                     {"text":"No.","goto":"no"},
                     {"text":"(only if 3 coins) Take the shard.","goto":"yes","if":"coins>=3"} ] },
  "yes": { "speaker":"The Void", "text":"Good.", "goto":"end" },
  "no":  { "speaker":"The Void", "text":"Coward.", "goto":"end" } } }
```

- [ ] **Step 6: Verify in-editor.** Temp `G` → `Dialogue.Play("_branch")`. Play, advance to the choices, arrow between them (screenshot shows the `▸` marker moving + cyan glow), pick one, confirm the following line matches the branch. With `GameState.TotalCoins < 3` the third choice is hidden; set it ≥3 (collect coins or a temp assignment) and confirm it appears. Remove temp `G`.

- [ ] **Step 7: Commit.**
```bash
git add -A && git commit -m "Add dialogue choices: manual nav, conditions, effects, branching"
```

---

## Task 6: Portrait slot

**Files:**
- Modify: `projects/INKBOUND/scripts/DialogueRunner.cs`
- Create: `projects/INKBOUND/assets/textures/portraits/void.png` (placeholder — a dim ink silhouette). Generate via the MCP pixel tools (`pixel_new`/`pixel_fill_rect`/`pixel_save`) or a small dark radial PNG.

- [ ] **Step 1: Add the portrait image element.** In `Build`, create `_portrait = Ui.CreateImage(_canvas)`; corner radius + a thin accent frame (a second image behind it, or just corner radius + a border color image). Reserve a square region on the box's left (~`BoxH - 2*Pad`).

- [ ] **Step 2: Wire load + layout shift.** In `GoTo`:
```csharp
if (!string.IsNullOrEmpty(_node.Portrait))
{
    Ui.SetImageTexture(_portrait, $"project://{_node.Portrait}"); // e.g. project://textures/portraits/void.png
    _portrait.SetActive(true);
    _bodyLeft = Pad + PortraitSize + Pad; // text column starts right of the portrait
}
else { _portrait.SetActive(false); _bodyLeft = Pad; }
```
Re-place `_speaker`/`_body` at `_bodyLeft`. (Confirm the exact texture VFS prefix like Task 1 — likely `project://textures/...`.)

- [ ] **Step 3: Verify.** Add `"portrait": "textures/portraits/void.png"` to a node in `_branch.json`. Play → the portrait shows on the left, text column narrows; a node without `portrait` hides it and text widens. Screenshot both.

- [ ] **Step 4: Commit.**
```bash
git add -A && git commit -m "Add dialogue portrait slot with placeholder art"
```

---

## Task 7: `ui_dialogue_text` material — whole-line rich-text effects

**Files:**
- Create: `projects/INKBOUND/assets/shaders/ui_dialogue_text.slang`
- Modify: `projects/INKBOUND/scripts/DialogueRunner.cs` (apply material to `_body` per node effect)

- [ ] **Step 1: Write the effect material.** Base it on `ui_glitch_text.slang` (same `ShapesPush`, `VSOutput`, `kShapeSdfGlyph` guard, `SdfCov`). Add `params.y = effectId`, `params.z = strength`, and branch the glyph treatment. Create `projects/INKBOUND/assets/shaders/ui_dialogue_text.slang`:

```hlsl
// INKBOUND dialogue body material. Per-glyph rich-text effects selected by params.y (InkEffect id).
// Masked to real glyph SDF geometry via the shared ui_shapes vertex (same contract as ui_glitch_text).
// params.x = time, params.y = effectId, params.z = strength; color0 = base (0 => glyph colour),
// color1 = accent. Offsets stay within the glyph SDF padding so sampling never bleeds neighbours.

[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_linearSampler;

struct ShapesPush
{
    float4   screenSize;
    uint64_t commandData;
    uint     pad0;
    uint     pad1;
    float4   params;   // x=time, y=effectId, z=strength
    float4   color0;
    float4   color1;
};
[[vk::push_constant]] ShapesPush pc;

struct VSOutput
{
    float4 position      : SV_Position;
    float4 color         : TEXCOORD0;
    float2 local         : TEXCOORD1;
    float2 localScale    : TEXCOORD2;
    float  cornerRadius  : TEXCOORD3;
    nointerpolation uint type        : TEXCOORD4;
    float2 texUV         : TEXCOORD5;
    nointerpolation uint textureSlot : TEXCOORD6;
    nointerpolation uint flags       : TEXCOORD7;
};
static const uint kShapeSdfGlyph = 4;

float hash11(float x) { return frac(sin(x * 12.9898) * 43758.5453); }

float SdfCov(uint slot, float2 uv)
{
    const float d = g_textures[slot].Sample(g_linearSampler, uv).r;
    const float w = clamp(fwidth(d) * 0.7, 0.001, 0.5);
    return smoothstep(0.5 - w, 0.5 + w, d);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    if (input.type != kShapeSdfGlyph) { return input.color; }

    const float time = pc.params.x;
    const uint  fx   = (uint)(pc.params.y + 0.5);
    const float amp  = pc.params.z <= 0.0 ? 1.0 : pc.params.z;

    uint texW, texH; g_textures[input.textureSlot].GetDimensions(texW, texH);
    const float2 texel = 1.0 / float2(max(texW, 1u), max(texH, 1u));

    // Per-glyph phase from screen position so motion differs glyph-to-glyph (blocky, not rigid).
    const float gphase = input.position.x * 0.15 + input.position.y * 0.07;

    float2 off = float2(0, 0);
    float  bright = 1.0;
    float  alphaMul = 1.0;
    float3 tint = float3(1, 1, 1);

    if (fx == 1) // shake
    {
        const float jx = hash11(floor(time * 30.0) + gphase) - 0.5;
        const float jy = hash11(floor(time * 30.0) + gphase + 7.0) - 0.5;
        off = float2(jx, jy) * texel * (2.5 * amp);
    }
    else if (fx == 2) // wave
    {
        off = float2(0, sin(time * 4.0 + gphase * 2.0) * texel.y * (3.0 * amp));
    }
    else if (fx == 3) // flicker
    {
        const float f = hash11(floor(time * 22.0) + gphase);
        bright = f > 0.88 ? 0.25 : (0.85 + 0.15 * f);
        alphaMul = f > 0.94 ? 0.4 : 1.0;
    }
    else if (fx == 4) // whisper: dim, slow breath, faint chroma bleed
    {
        bright = 0.6 + 0.12 * sin(time * 1.6 + gphase);
        off = float2(sin(time * 0.8 + gphase) * texel.x * 0.6 * amp, 0);
        tint = lerp(float3(1,1,1), pc.color1.rgb, 0.15);
    }
    else if (fx == 5) // glitch: chromatic split + row jump (condensed from ui_glitch_text)
    {
        const float rny = hash11(floor(input.position.y * 0.5) + floor(time * 18.0));
        const float jump = (rny > 0.93 ? 1.0 : 0.0);
        off = float2((rny - 0.5) * texel.x * (1.5 + 9.0 * jump), 0);
    }

    const float cov = SdfCov(input.textureSlot, input.texUV + off);
    if (cov <= 0.003) { discard; }

    float3 base = dot(pc.color0.rgb, float3(1,1,1)) > 0.001 ? pc.color0.rgb : input.color.rgb;
    float3 rgb = base * tint * bright;

    if (fx == 5) // add the accent fringe on the split
    {
        const float split = texel.x * 1.6;
        const float cr = SdfCov(input.textureSlot, input.texUV + off + float2(split, 0));
        rgb += pc.color1.rgb * max(cr - cov, 0.0) * 1.1;
    }

    return float4(rgb, input.color.a * cov * alphaMul);
}
```

- [ ] **Step 2: Apply the material per node effect.** In `DialogueRunner.GoTo`, after setting the body text:
```csharp
Ui.SetMaterial(_body, "ui_dialogue_text");
Ui.SetMaterialColors(_body, Vector4.Zero, Accent); // 0 base => use glyph colour, accent for fringe
```
and each frame in `OnUpdate`:
```csharp
float strength = 0.6f + 0.4f * GameSettings.InkGlow;
Ui.SetMaterialParams(_body, new Vector4(Time.UnscaledTime, (float)_node.Effect, strength, 0f));
```
(For `InkEffect.Normal` you may skip the material to save a pass — set it only when `_node.Effect != Normal`.)

- [ ] **Step 3: Build shaders + verify.**
```bash
cmake --build D:/AetherCore/build/vs2022-msvc --target App_CompileShaders --config Debug
```
Set `"effect":"whisper"` (and try `shake`) on a node. Play → the whole line visibly shakes/whispers per-glyph. Screenshot; confirm no glyph bleed (neighbours stay clean).

- [ ] **Step 4: Commit.**
```bash
git add -A && git commit -m "Add ui_dialogue_text material with per-glyph rich-text effects"
```

---

## Task 8: Inline effect spans (per-run layout)

**Files:**
- Modify: `projects/INKBOUND/scripts/DialogueRunner.cs`

**Approach:** Render the body as one text element per `TextRun`, laid out inline using IBM Plex **Mono**'s fixed cell width. The typewriter reveals a global character count across runs in order. Tagged runs wear `ui_dialogue_text` at their run effect; untagged runs render plain (or Normal).

- [ ] **Step 1: Calibrate the mono cell width.** Add `private float CellW => _body != null ? BodyFontSize * MonoAdvance : 0f;` with `const float MonoAdvance = 0.6f; const float BodyFontSize = 26f;`. (IBM Plex Mono advance ≈ 0.6 em; verify by rendering a known-length run and comparing `Ui.GetRect` width — adjust the constant so `CellW * charCount ≈ measured width`.)

- [ ] **Step 2: Build run elements on `GoTo`.** Replace the single `_body` reveal with a pooled `Entity[] _runUi` + per-run metadata (`start char index`, `effect`, `x column`, `row`). Lay out runs left-to-right wrapping at `MaxCols = floor((boxW - textLeft - Pad) / CellW)`; advance columns by each run's char count; wrap to the next row at the column limit (breaking a run across rows if needed). Apply `ui_dialogue_text` material to runs whose effect != Normal.

- [ ] **Step 3: Reveal across runs.** Keep the global `_reveal` char counter; for each run show `Substring(0, clamp(revealed - runStart, 0, runLen))`; hide runs not yet reached. Full-shown when `_reveal >= plainText.Length`.

- [ ] **Step 4: Verify.** Node text `"You should not have [shake]come this far[/shake] down, [whisper]little light[/whisper]."` Play → only the tagged spans animate, the rest is calm; the line wraps cleanly; typewriter crosses runs in order. Screenshot.

- [ ] **Step 5: Commit.**
```bash
git add -A && git commit -m "Add inline rich-text effect spans with mono run layout"
```

---

## Task 9: Animation polish (nothing appears in one frame)

**Files:**
- Modify: `projects/INKBOUND/scripts/DialogueRunner.cs`

Use `StartCoroutine` + `Time.UnscaledTime`-based waits (do **not** use `WaitForSeconds` if it keys off scaled time while paused — drive tweens from `UnscaledTime` deltas in `OnUpdate` or a coroutine that polls `Time.UnscaledTime`). Each item below is a small tween helper.

- [ ] **Step 1: Box open/close.** On `Begin`, animate the panel from an off-screen-low + alpha 0 to seated over ~0.28s (ease-out), with a quick accent-rule wipe. On `End`, reverse over ~0.22s, then `Hide`. Keep the world paused across the close tween.

- [ ] **Step 2: Line-to-line cross-fade.** On `Next`/`Choose`, fade the current body/speaker out (~0.12s), swap node, fade speaker + portrait in, then start the typewriter — not an instant text swap.

- [ ] **Step 3: Staggered choice reveal.** After the line finishes typing, reveal choices one at a time (~0.06s apart), each sliding up a few px + fading in.

- [ ] **Step 4: Portrait/speaker fade-in.** On node change, fade the portrait + speaker from alpha 0 over ~0.15s.

- [ ] **Step 5: Verify.** Play a multi-line branching conversation; watch (screenshots at intervals or a short manual capture) that the box slides in, lines cross-fade, choices stagger, portrait fades — no single-frame pops. Confirm all animate while `Time.IsPaused`.

- [ ] **Step 6: Commit.**
```bash
git add -A && git commit -m "Animate dialogue box, line transitions, and choice reveal"
```

---

## Task 10: `DialogueTrigger` component — zones, interact, prompt

**Files:**
- Create: `projects/INKBOUND/scripts/DialogueTrigger.cs`

**Interfaces:**
- Consumes: `Dialogue.Play` (Task 4), `EntityScript.OnTriggerEnter2D` (auto-events).

- [ ] **Step 1: Write the trigger.** Create `projects/INKBOUND/scripts/DialogueTrigger.cs`:

```csharp
using AetherCore;

namespace AetherGame;

/// <summary>Starts a conversation from the world. Attach to an entity that has a 2D trigger collider.
/// Zone mode (RequireInteract=false) fires on player enter. Interact mode shows a "> read" prompt while
/// the player is inside and fires on the interact key. Script-driven dialogue needs no trigger — call
/// Dialogue.Play("id") directly.</summary>
public sealed class DialogueTrigger : EntityScript
{
    public string DialogueId = "";
    public bool Once = true;
    public bool RequireInteract = false;

    private bool _fired;
    private bool _playerInside;
    private Entity _prompt;
    private const Key InteractKey = Key.E; // confirm no collision with PlayerController bindings

    public override void OnAttach()
    {
        Physics2D.SetTrigger(Self, true);
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        if (!IsPlayer(other)) return;
        _playerInside = true;
        if (!RequireInteract) { TryFire(); }
        else { ShowPrompt(true); }
    }

    public override void OnTriggerExit2D(Entity other)
    {
        if (!IsPlayer(other)) return;
        _playerInside = false;
        ShowPrompt(false);
    }

    public override void OnUpdate(float dt)
    {
        if (RequireInteract && _playerInside && !Dialogue.IsActive && Input.IsKeyPressed(InteractKey))
        {
            TryFire();
            ShowPrompt(false);
        }
    }

    private void TryFire()
    {
        if (_fired && Once) return;
        if (string.IsNullOrEmpty(DialogueId)) { Log.Warn("[INKBOUND] DialogueTrigger has no DialogueId"); return; }
        Dialogue.Play(DialogueId);
        _fired = true;
    }

    private static bool IsPlayer(Entity e) => e.Name == "Player"; // match the project's player entity name

    private void ShowPrompt(bool on)
    {
        if (on && !_prompt.IsValid)
        {
            _prompt = Ui.CreateText(default, "> read");
            Ui.SetFontSize(_prompt, 18f);
            Ui.SetTextColor(_prompt, GameSettings.Accent);
            Ui.SetAnchors(_prompt, new System.Numerics.Vector2(0.5f, 0f), new System.Numerics.Vector2(0.5f, 0f));
            Ui.SetPivot(_prompt, new System.Numerics.Vector2(0.5f, 0f));
            Ui.SetRect(_prompt, 0f, 120f, 200f, 30f);
        }
        if (_prompt.IsValid) { _prompt.SetActive(on); }
    }

    public override void OnDetach() { if (_prompt.IsValid) _prompt.Destroy(); }
}
```

> Confirm `IsPlayer` against how the player entity is identified in INKBOUND (name, tag, or the `AetherInk`/`PlayerController` owner). Confirm `InteractKey` doesn't clash with movement/jump/draw in `PlayerController`.

- [ ] **Step 2: Verify all three paths.**
  - **Zone:** place a `DialogueTrigger` (with a box trigger collider) in `Level1`, `RequireInteract=false`, `DialogueId="_branch"`; walk the player in (drive with movement keys) → dialogue starts.
  - **Interact:** second trigger `RequireInteract=true`; walk in → `> read` prompt shows; press `E` → starts; leave → prompt hides.
  - **Script:** from a level script `OnAttach`, `Dialogue.Play("_linear")` → plays on load.
  Screenshot each.

- [ ] **Step 3: Commit.**
```bash
git add -A && git commit -m "Add DialogueTrigger: zone, interact-key, and prompt"
```

---

## Task 11: Sample conversation + wire into Level1 + end-to-end

**Files:**
- Create: `projects/INKBOUND/assets/dialogue/intro.json`
- Modify: `Level1` scene (add a DialogueRunner host if not persistent from a prior level, + an intro trigger)
- Delete: the temporary `_linear.json` / `_branch.json` test files

- [ ] **Step 1: Author `intro.json`** using a portrait, a whole-line `effect`, an inline `[shake]`/`[whisper]` span, and a branch with an `if`/`set` — a real opening beat for descending into the first level. ASCII only.

- [ ] **Step 2: Wire it.** Ensure exactly one `DialogueRunner` exists in gameplay (persistent via `DontDestroyOnLoad`, or one per level scene). Add a zone `DialogueTrigger` near Level1's start firing `intro` once.

- [ ] **Step 3: Full end-to-end playtest.** Fresh editor, load `Level1`, `play`. Walk into the trigger → intro plays: box slides in, portrait fades, whisper line animates, inline shake span animates, choices stagger + navigate by keyboard, branch resolves, box closes, gameplay resumes, player can move + draw ink. Screenshot the key beats. Confirm `EngineTests` still green (only Task 1 touched C++):
```bash
cmake --build D:/AetherCore/build/vs2022-msvc --target EngineTests --config Debug && ctest --test-dir D:/AetherCore/build/vs2022-msvc -R Engine
```

- [ ] **Step 4: Remove temp test conversations.**
```bash
rm D:/AetherCore/projects/INKBOUND/assets/dialogue/_linear.json D:/AetherCore/projects/INKBOUND/assets/dialogue/_branch.json
```

- [ ] **Step 5: Revert any stray editor scene auto-migrations** (stable-node-id / `points=[]` churn) so the commit is focused, keeping only the intentional trigger/host additions to `Level1`.

- [ ] **Step 6: Commit + finish.**
```bash
git add -A && git commit -m "Wire intro dialogue into Level1 with portrait, effects, and a branch"
```
Then use **superpowers:finishing-a-development-branch** to fast-forward merge + push to `master`.

---

## Self-review notes

- **Spec coverage:** engine hook (T1), JSON model + conditions (T2-3), runner/typewriter/pause (T4), choices/branching (T5), portrait (T6), rich-text material + inline spans (T7-8), animation (T9), triggers all three (T10), sample + e2e (T11). All spec sections map to a task.
- **Boundary:** only T1 touches engine/SDK, and it is generic (`Assets.ReadText`). Everything else is under `projects/INKBOUND/`.
- **Open verifications deliberately deferred to their task** (not placeholders): the VFS prefix (`DlgRoot`, pinned in T1 Step 7), the mono cell-width constant (T8 Step 1), the player-identity check + interact key (T10 Step 1), and the exact box anchor math (T4 Step 2 note). Each has a concrete verification step.
- **Type consistency:** `InkEffect`, `TextRun`, `DialogueNode/Choice/Graph`, `DialogueState`, `Dialogue`, `DialogueRunner`, `DialogueTrigger` names are used identically across tasks.
