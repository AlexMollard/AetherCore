# INKBOUND Save, Slots & Progression — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give INKBOUND a persistent 3-slot save profile so completion, unlocks, best coins, mid-level checkpoint resume, and an opt-in per-level time-trial all survive a quit — and make Level Select's locking real.

**Architecture:** Pure project C# under `projects/INKBOUND/scripts/`, mirroring the proven `GameSettings.cs` persistence pattern (`System.IO` + `System.Text.Json` into `%LocalAppData%/AetherCore/INKBOUND/`). Two new data/IO units (`SaveProfile`, `SaveSystem`), a per-level self-identifying marker (`LevelInfo`), and wiring into the existing `GameState`, `Checkpoint`, `GoalFlag`, `TitleScreen`, `MenuController`, `LevelSelectScreen`, plus a new `SlotSelectScreen`.

**Tech Stack:** C# (net10.0, nullable enabled), `System.Text.Json`, the AetherCore SDK (`Scene`, `Ui`, `Input`, `Log`, `EntityScript`). Runtime is CoreCLR; the editor compiles `AetherGame.csproj` and hot-loads `AetherGame.dll`.

## Global Constraints

- **No engine/SDK changes.** Everything lives under `projects/INKBOUND/`. If a task seems to need an engine API, stop and re-scope.
- **No unit-test runner exists** for project game logic (no xunit/nunit in the repo). Verification per task = (a) `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug` compiles clean, and (b) a scripted playtest via the aethercore MCP with the concrete observable outcome named in the task. Where a task adds pure logic, it also carries a temporary `SelfTest()` console check that is removed before its commit.
- **UI text is ASCII-only** (multibyte renders as mojibake). Use `-` not `—`, spell things out.
- **Settings persistence is unchanged** — `settings.json` stays global. Save profiles are new, separate files.
- **The tracked collectible is coins** (`GameState.Coins`). Aether crystals stay a respawning resource; do not count them.
- **Level keys are exactly** `Level1 Level2 Level3 Level4`, in that unlock order.
- **Menu entity names are a hard contract** between scripts and scenes — the scene must use the exact `name = '...'` values a script calls `Scene.Find("...")` on.
- **Commit style:** plain imperative subject, no prefixes/attribution (per repo CLAUDE.md).
- After the whole plan lands and is verified, fast-forward merge + push to `master` (per project convention).

## File Structure

**New scripts**
- `scripts/SaveProfile.cs` — per-slot serializable data + pure derivation helpers (unlock rule, record mutators). No engine calls.
- `scripts/SaveSystem.cs` — owns the three slot files; load-all, active slot, save-active, wipe. Mirrors `GameSettings` IO.
- `scripts/LevelInfo.cs` — tiny marker script; sets `GameState.CurrentLevel` from an authored `Key`.
- `scripts/SlotSelectScreen.cs` — the new slot-select menu screen (implements `IMenuScreen`).

**Modified scripts**
- `scripts/GameState.cs` — active-profile binding, `CurrentLevel`, trial timing, completion recording.
- `scripts/Checkpoint.cs` — authored `Index`, record-on-hit, resume-by-index.
- `scripts/PlayerController.cs` — `TeleportTo`, trial timer tick, resume plumb.
- `scripts/MenuController.cs` — `SlotSelect` state + root wiring.
- `scripts/TitleScreen.cs` — `descend`/`return` route into SlotSelect with a mode.
- `scripts/LevelSelectScreen.cs` — per-active-slot lock/DONE/records + time-trial toggle.
- `scripts/HudController.cs` — trial clock readout.

**Modified scenes/assets**
- `scenes/Menu.scene.toml` — add `SlotSelectRoot` subtree (3 slot rows + labels + confirm prompt).
- `scenes/Level1..Level4.scene.toml` — add a `LevelInfo` entity (`Key`), and an `Index` on each `Checkpoint`.
- `assets/prefabs/GameHud.prefab.toml` — add a `HudTrialClock` UI text element.

---

## Task 1: Persistence core — `SaveProfile` + `SaveSystem`

**Files:**
- Create: `projects/INKBOUND/scripts/SaveProfile.cs`
- Create: `projects/INKBOUND/scripts/SaveSystem.cs`

**Interfaces:**
- Produces:
  - `SaveProfile` with `bool Exists`, `string FurthestLevel`, `Dictionary<string,LevelRecord> Levels`, and methods `LevelRecord Level(string)`, `bool IsUnlocked(string)`, `void RecordCompletion(string key,int coins)`, `void RecordCheckpoint(string key,int index)`, `void RecordBestTime(string key,float seconds)`, `int CompletedCount()`. `static string[] LevelKeys`.
  - `LevelRecord` with `bool Completed`, `int BestCoins`, `float? BestTimeSeconds`, `int FurthestCheckpoint`.
  - `SaveSystem` (static): `const int SlotCount=3`, `void EnsureLoaded()`, `void LoadAll()`, `SaveProfile Slot(int)`, `int ActiveIndex`, `SaveProfile? Active`, `void SetActive(int)`, `void SaveActive()`, `void Wipe(int)`.

- [ ] **Step 1: Write `SaveProfile.cs`**

```csharp
using System;
using System.Collections.Generic;

namespace AetherGame;

/// <summary>Per-slot persisted progression. Plain data plus pure derivation helpers - no engine
/// calls, so it reasons and serialises cleanly with System.Text.Json. Unlock is DERIVED from
/// completion (never stored): Level1 is always open; every later level opens once the previous is
/// completed.</summary>
public sealed class SaveProfile
{
    /// <summary>Ship order of levels; index order defines the linear unlock chain.</summary>
    public static readonly string[] LevelKeys = { "Level1", "Level2", "Level3", "Level4" };

    public bool Exists { get; set; }
    public string FurthestLevel { get; set; } = "Level1";
    public Dictionary<string, LevelRecord> Levels { get; set; } = new();

    public LevelRecord Level(string key)
    {
        if (!Levels.TryGetValue(key, out LevelRecord? r)) { r = new LevelRecord(); Levels[key] = r; }
        return r;
    }

    public bool IsUnlocked(string key)
    {
        int i = Array.IndexOf(LevelKeys, key);
        if (i < 0) return false;      // unknown key (e.g. Sandbox) never gates through here
        if (i == 0) return true;      // Level1 always open
        return Level(LevelKeys[i - 1]).Completed;
    }

    public void RecordCompletion(string key, int coins)
    {
        LevelRecord r = Level(key);
        r.Completed = true;
        if (coins > r.BestCoins) r.BestCoins = coins;
        r.FurthestCheckpoint = 0;     // finished: no partial-progress checkpoint to resume to
        Exists = true;
    }

    public void RecordCheckpoint(string key, int index)
    {
        LevelRecord r = Level(key);
        if (index > r.FurthestCheckpoint) r.FurthestCheckpoint = index;
        if (Array.IndexOf(LevelKeys, key) >= Array.IndexOf(LevelKeys, FurthestLevel)) FurthestLevel = key;
        Exists = true;
    }

    public void RecordBestTime(string key, float seconds)
    {
        LevelRecord r = Level(key);
        if (r.BestTimeSeconds == null || seconds < r.BestTimeSeconds.Value) r.BestTimeSeconds = seconds;
    }

    public int CompletedCount()
    {
        int n = 0;
        foreach (string k in LevelKeys) if (Level(k).Completed) n++;
        return n;
    }
}

public sealed class LevelRecord
{
    public bool Completed { get; set; }
    public int BestCoins { get; set; }
    public float? BestTimeSeconds { get; set; }
    public int FurthestCheckpoint { get; set; }   // 0 = no checkpoint reached (resume at level start)
}
```

- [ ] **Step 2: Write `SaveSystem.cs`**

```csharp
using System;
using System.IO;
using System.Text.Json;
using AetherCore;

namespace AetherGame;

/// <summary>Owns on-disk save state: three slot files under LocalAppData, mirroring GameSettings'
/// pattern. Loads once on first menu entry; the active slot receives every autosave.</summary>
public static class SaveSystem
{
    public const int SlotCount = 3;

    private static readonly SaveProfile[] s_slots = new SaveProfile[SlotCount];
    private static bool s_loaded;

    public static int ActiveIndex { get; private set; } = -1;
    public static SaveProfile? Active => ActiveIndex >= 0 ? s_slots[ActiveIndex] : null;
    public static SaveProfile Slot(int i) => s_slots[i];

    private static string Dir()
    {
        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "AetherCore", "INKBOUND");
        Directory.CreateDirectory(dir);
        return dir;
    }

    private static string PathFor(int i) => Path.Combine(Dir(), $"slot{i}.json");

    public static void EnsureLoaded() { if (!s_loaded) { LoadAll(); s_loaded = true; } }

    public static void LoadAll() { for (int i = 0; i < SlotCount; i++) s_slots[i] = LoadOne(i); }

    private static SaveProfile LoadOne(int i)
    {
        try
        {
            string p = PathFor(i);
            if (!File.Exists(p)) return new SaveProfile { Exists = false };
            SaveProfile? prof = JsonSerializer.Deserialize<SaveProfile>(File.ReadAllText(p));
            return prof ?? new SaveProfile { Exists = false };
        }
        catch (Exception e)
        {
            Log.Warn($"[INKBOUND] slot {i} load failed: {e.Message}");
            return new SaveProfile { Exists = false };
        }
    }

    public static void SetActive(int i) => ActiveIndex = i;

    public static void SaveActive()
    {
        if (ActiveIndex < 0) return;
        try { File.WriteAllText(PathFor(ActiveIndex), JsonSerializer.Serialize(s_slots[ActiveIndex])); }
        catch (Exception e) { Log.Warn($"[INKBOUND] slot {ActiveIndex} save failed: {e.Message}"); }
    }

    public static void Wipe(int i)
    {
        s_slots[i] = new SaveProfile { Exists = false };
        try { File.Delete(PathFor(i)); } catch { /* absent = already wiped */ }
    }
}
```

- [ ] **Step 3: Add a temporary self-test and run it via the editor console**

Temporarily add this method to `SaveSystem` and call `SaveSystem.SelfTest()` at the top of `MenuController.OnAttach` (before `GameSettings.Load()`):

```csharp
public static void SelfTest()
{
    var p = new SaveProfile();
    bool ok = p.IsUnlocked("Level1") && !p.IsUnlocked("Level2");
    p.RecordCompletion("Level1", 5);
    ok &= p.IsUnlocked("Level2") && p.Level("Level1").BestCoins == 5 && p.CompletedCount() == 1;
    p.RecordCompletion("Level1", 3);                 // lower coins must NOT lower the best
    ok &= p.Level("Level1").BestCoins == 5;
    p.RecordCheckpoint("Level2", 2);
    ok &= p.FurthestLevel == "Level2" && p.Level("Level2").FurthestCheckpoint == 2;
    p.RecordBestTime("Level2", 90f); p.RecordBestTime("Level2", 120f);   // min only
    ok &= p.Level("Level2").BestTimeSeconds == 90f;
    string json = System.Text.Json.JsonSerializer.Serialize(p);
    var back = System.Text.Json.JsonSerializer.Deserialize<SaveProfile>(json)!;
    ok &= back.Level("Level1").BestCoins == 5 && back.FurthestLevel == "Level2";
    Log.Info($"[INKBOUND] SaveSystem.SelfTest {(ok ? "PASS" : "FAIL")}");
}
```

Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug`
Expected: `Build succeeded`, 0 errors.

Then in the running editor (Launcher on `AETHER_CONTROL_PORT=8787`): open INKBOUND, load the Menu scene, press Play, and read the console.
Verify via MCP: `get_console_log` contains `SaveSystem.SelfTest PASS`.

- [ ] **Step 4: Remove the self-test**

Delete the `SelfTest()` method and its call in `MenuController.OnAttach`.
Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug` → `Build succeeded`.

- [ ] **Step 5: Commit**

```bash
git add projects/INKBOUND/scripts/SaveProfile.cs projects/INKBOUND/scripts/SaveSystem.cs
git commit -m "Add INKBOUND save profile and slot persistence"
```

---

## Task 2: Level identity + completion recording

**Files:**
- Create: `projects/INKBOUND/scripts/LevelInfo.cs`
- Modify: `projects/INKBOUND/scripts/GameState.cs`
- Modify: `projects/INKBOUND/scenes/Level1.scene.toml`, `Level2.scene.toml`, `Level3.scene.toml`, `Level4.scene.toml`

**Interfaces:**
- Consumes: `SaveSystem.Active`, `SaveProfile.RecordCompletion` (Task 1).
- Produces: `GameState.CurrentLevel` (string), `GameState.TrialMode` (bool), `GameState.TrialElapsed` (float), and completion recording inside `GameState.Win()`. `LevelInfo` script type with public `string Key`.

- [ ] **Step 1: Write `LevelInfo.cs`**

```csharp
using AetherCore;

namespace AetherGame;

/// <summary>Authored once per level scene. Tells GameState which level key is running so completion,
/// checkpoints and best-times record against the right save slot. Empty Key = untracked (Sandbox).</summary>
public sealed class LevelInfo : EntityScript
{
    public string Key = "";

    public override void OnAttach() => GameState.CurrentLevel = Key;
}
```

- [ ] **Step 2: Extend `GameState.cs`** — add fields and completion recording.

Add these fields near the existing statics:

```csharp
    /// <summary>Scene key of the running level (set by LevelInfo); "" = untracked.</summary>
    public static string CurrentLevel = "";

    /// <summary>A time-trial run is in progress (set by Level Select).</summary>
    public static bool TrialMode;

    /// <summary>Elapsed trial seconds; ticked by PlayerController while unpaused, reset per level.</summary>
    public static float TrialElapsed;
```

In `BeginLevel()`, reset the trial clock (leave `TrialMode` alone — it is chosen at descend time):

```csharp
    public static void BeginLevel()
    {
        Coins = 0;
        Won = false;
        NextSceneQueued = false;
        TrialElapsed = 0.0f;
    }
```

In `Win()`, after `Won = true;`, record into the active slot:

```csharp
        Won = true;
        if (CurrentLevel.Length > 0 && SaveSystem.Active != null)
        {
            SaveSystem.Active.RecordCompletion(CurrentLevel, Coins);
            if (TrialMode) SaveSystem.Active.RecordBestTime(CurrentLevel, TrialElapsed);
            SaveSystem.SaveActive();
        }
        Log.Info($"[INKBOUND] Level complete with {Coins} coins ({TotalCoins} this run)!");
```

(Remove the old trailing `Log.Info` line so it is not duplicated.)

- [ ] **Step 3: Author a `LevelInfo` entity in each level scene**

For each of `Level1..Level4`, append one `[[entities]]` block (via the editor Hierarchy "Create Empty" + Add Script `LevelInfo`, or hand-append then `rm` the stale `.scene.bin` per the cooked-bin gotcha). Set `Key` to the matching level. Minimal hand-authored block (use a fresh unique `node` id — the editor assigns one automatically if you create it in-editor):

```toml
[[entities]]
name = 'LevelInfo'
position = [ 0.0, 0.0, 0.0 ]
euler = [ 0.0, 0.0, 0.0 ]
scale = [ 1.0, 1.0, 1.0 ]

    [[entities.scripts]]
    type = 'LevelInfo'
    [entities.scripts.properties]
    Key = 'Level1'
```

Do the same for `Level2`/`Level3`/`Level4` with the matching `Key`. Leave `Sandbox` untracked (no `LevelInfo`).

- [ ] **Step 4: Build**

Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug`
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 5: Verify completion recording via playtest**

In the editor: open INKBOUND, load `Level1`, Play. Then, via MCP, drive the player into the goal (either play the level or `set_transform` the player onto the goal flag), and confirm the win.
Verify: `get_console_log` shows `Level complete`; then check `%LocalAppData%/AetherCore/INKBOUND/slot0.json` does NOT yet exist (no active slot chosen in direct Play) — recording is guarded by `SaveSystem.Active != null`, so direct-Play of a level records nothing. This guard is intended; full recording is exercised end-to-end in Task 7.

- [ ] **Step 6: Commit**

```bash
git add projects/INKBOUND/scripts/LevelInfo.cs projects/INKBOUND/scripts/GameState.cs projects/INKBOUND/scenes/Level1.scene.toml projects/INKBOUND/scenes/Level2.scene.toml projects/INKBOUND/scenes/Level3.scene.toml projects/INKBOUND/scenes/Level4.scene.toml
git commit -m "Record level completion into the active save slot"
```

---

## Task 3: Checkpoint index, recording, and resume

**Files:**
- Modify: `projects/INKBOUND/scripts/Checkpoint.cs`
- Modify: `projects/INKBOUND/scripts/PlayerController.cs`
- Modify: `projects/INKBOUND/scripts/GameState.cs`
- Modify: level scenes that contain checkpoints (author `Index` per checkpoint)

**Interfaces:**
- Consumes: `SaveSystem.Active`, `SaveProfile.RecordCheckpoint`, `GameState.CurrentLevel` (Tasks 1-2).
- Produces:
  - `GameState.ResumeLevel` (string), `GameState.ResumeCheckpoint` (int), `bool GameState.ConsumeResume(string level, int index)`.
  - `PlayerController.TeleportTo(Vector3)`.
  - `Checkpoint.Index` (int, authored).

- [ ] **Step 1: Add resume plumbing to `GameState.cs`**

```csharp
    /// <summary>Pending resume target set by 'return' from the menu; consumed once by the matching
    /// checkpoint on level load. "" / 0 = start the level from its authored spawn.</summary>
    public static string ResumeLevel = "";
    public static int ResumeCheckpoint;

    /// <summary>True exactly once, for the checkpoint whose level+index match the pending resume.
    /// Clears the pending resume so it fires a single time.</summary>
    public static bool ConsumeResume(string level, int index)
    {
        if (ResumeLevel == level && ResumeCheckpoint == index && index > 0)
        {
            ResumeLevel = "";
            ResumeCheckpoint = 0;
            return true;
        }
        return false;
    }
```

- [ ] **Step 2: Add `TeleportTo` to `PlayerController.cs`**

```csharp
    /// <summary>Hard-move the player (used by checkpoint resume) and reset the checkpoint anchor there.</summary>
    public void TeleportTo(Vector3 position)
    {
        _spawn = position;
        Self.Position = position;
        Physics2D.SetLinearVelocity(Self, Vector2.Zero);
    }
```

- [ ] **Step 3: Tick the trial clock in `PlayerController.OnUpdate`**

Immediately after the `if (Time.IsPaused || Dialogue.IsActive) return;` guard and before the `if (GameState.Won)` block, add:

```csharp
        if (GameState.TrialMode && !GameState.Won) GameState.TrialElapsed += deltaTime;
```

(Placed after the pause/dialogue guard so the clock naturally freezes during dialogue and the pause menu, per the design.)

- [ ] **Step 4: Extend `Checkpoint.cs`** — authored index, record on hit, resume on first update.

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>A checkpoint flag. First pass-through becomes the respawn point and lights up. Its Index
/// (authored, ascending along the level) is written to the active save slot so 'return' can resume
/// here.</summary>
public sealed class Checkpoint : EntityScript
{
    public float SpawnYOffset = 0.5f;
    /// <summary>Ascending checkpoint ordinal within the level (1, 2, 3...). 0 = not resumable.</summary>
    public int Index = 1;

    private bool _active;
    private bool _resumeChecked;

    public override void OnAttach()
    {
        Physics2D.EnableEvents(Self);
        SpriteRenderer.SetTint(Self, new Vector4(0.6f, 0.6f, 0.6f, 1.0f)); // dim until reached
    }

    public override void OnUpdate(float deltaTime)
    {
        // Resume lands here on the first frame the player exists. Wait a frame if the player has not
        // attached yet (ConsumeResume only fires for the matching level+index, exactly once).
        if (_resumeChecked) return;
        PlayerController? p = PlayerController.Instance;
        if (p == null) return;
        _resumeChecked = true;
        if (GameState.ConsumeResume(GameState.CurrentLevel, Index))
        {
            Vector3 here = Self.Position;
            p.TeleportTo(new Vector3(here.X, here.Y + SpawnYOffset, here.Z));
            Light();
        }
    }

    public override void OnTriggerEnter2D(Entity other)
    {
        PlayerController? player = PlayerController.Instance;
        if (_active || player == null || other.Id != player.Self.Id) return;
        Vector3 here = Self.Position;
        player.SetCheckpoint(new Vector3(here.X, here.Y + SpawnYOffset, here.Z));
        Light();
        if (GameState.CurrentLevel.Length > 0 && SaveSystem.Active != null)
        {
            SaveSystem.Active.RecordCheckpoint(GameState.CurrentLevel, Index);
            SaveSystem.SaveActive();
        }
        Log.Info($"[INKBOUND] Checkpoint {Index} reached!");
    }

    private void Light()
    {
        _active = true;
        SpriteRenderer.SetTint(Self, new Vector4(0.4f, 1.0f, 0.5f, 1.0f)); // lit green
    }
}
```

- [ ] **Step 5: Author ascending `Index` on each checkpoint**

In each level scene, for every entity whose script `type = 'Checkpoint'`, add `Index` to its `[entities.scripts.properties]` (1 for the first checkpoint along the level, 2 for the next, etc.):

```toml
    [[entities.scripts]]
    type = 'Checkpoint'
    [entities.scripts.properties]
    Index = 1
```

Find them first: `grep -n "type = 'Checkpoint'" projects/INKBOUND/scenes/*.scene.toml`. Number them left-to-right by the checkpoint's X position. Delete stale `.scene.bin` for any hand-edited scene.

- [ ] **Step 6: Build + verify**

Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug` → `Build succeeded`.
Playtest (direct Play of a level with checkpoints): walk the player through a checkpoint; `get_console_log` shows `Checkpoint 1 reached!`. (Slot resume is exercised in Task 7.)

- [ ] **Step 7: Commit**

```bash
git add projects/INKBOUND/scripts/Checkpoint.cs projects/INKBOUND/scripts/PlayerController.cs projects/INKBOUND/scripts/GameState.cs projects/INKBOUND/scenes
git commit -m "Persist and resume from the furthest checkpoint per slot"
```

---

## Task 4: SlotSelect screen + descend/return routing

**Files:**
- Create: `projects/INKBOUND/scripts/SlotSelectScreen.cs`
- Modify: `projects/INKBOUND/scripts/MenuController.cs`
- Modify: `projects/INKBOUND/scripts/TitleScreen.cs`
- Modify: `projects/INKBOUND/scenes/Menu.scene.toml`

**Interfaces:**
- Consumes: `SaveSystem` (Task 1), `MenuController.Go`, `ScreenRegistry`, `IMenuScreen` (existing).
- Produces:
  - `MenuScreen.SlotSelect` enum value.
  - `SlotSelectScreen` implementing `IMenuScreen`, with a static entry point `SlotSelectScreen.Open(bool resumeOnly)` that MenuController/TitleScreen call to set the mode before switching.
  - Menu scene root `SlotSelectRoot` and child entities `Slot0..Slot2` (UISelectable, group `slots`), `SlotText0..SlotText2` (UI Text), plus `SlotConfirm`, `SlotConfirmYes`, `SlotConfirmNo`.

- [ ] **Step 1: Add the `SlotSelect` state to `MenuController.cs`**

In `enum MenuScreen`, add `SlotSelect`:

```csharp
public enum MenuScreen { Title, LevelSelect, Settings, SlotSelect }
```

Add a root field and wire it in `OnAttach`/`ShowInstant`/`ActiveScreen`:

```csharp
    private Entity _titleRoot, _levelRoot, _settingsRoot, _slotRoot;
```
In `OnAttach` after the other `Scene.Find` calls, add `SaveSystem.EnsureLoaded();` and `_slotRoot = Scene.Find("SlotSelectRoot");`
In `ShowInstant`, add: `if (_slotRoot.IsValid) _slotRoot.SetActive(s == MenuScreen.SlotSelect);`
In `ActiveScreen`, add the case:

```csharp
        MenuScreen.SlotSelect => ScreenRegistry<SlotSelectScreen>.Get("SlotSelectRoot"),
```

- [ ] **Step 2: Write `SlotSelectScreen.cs`**

```csharp
using System;
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>The 3-slot save screen, entered by both 'descend' (new-run mode) and 'return'
/// (resume-only mode). New-run mode lets you start/overwrite any slot (used slots ask to confirm);
/// resume mode only lets you continue a used slot. Styled with ui_ink_ui to match the menu suite.</summary>
public sealed class SlotSelectScreen : EntityScript, IMenuScreen
{
    private static bool s_resumeOnly;
    /// <summary>Set the mode, then switch to the screen. Called from Title.</summary>
    public static void Open(bool resumeOnly)
    {
        s_resumeOnly = resumeOnly;
        MenuController.Instance?.Go(MenuScreen.SlotSelect);
    }

    private readonly Entity[] _slots = new Entity[SaveSystem.SlotCount];
    private readonly Entity[] _texts = new Entity[SaveSystem.SlotCount];
    private Entity _confirm, _confirmYes, _confirmNo;
    private int _pendingOverwrite = -1;   // slot awaiting overwrite confirmation, -1 = none
    private float _t;

    private static readonly Vector4 Cyan = GameSettings.Accent;
    private static readonly Vector4 Dim = new(0.106f, 0.125f, 0.188f, 1f);
    private static readonly Vector4 InkEdge = new(0.02f, 0.06f, 0.09f, 1f);
    private static readonly Vector4 TextMuted = new(0.6f, 0.63f, 0.7f, 1f);
    private static readonly Vector4 TextOn = new(0.04f, 0.06f, 0.10f, 1f);

    public override void OnAttach()
    {
        ScreenRegistry<SlotSelectScreen>.Register("SlotSelectRoot", this);
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            _slots[i] = Scene.Find($"Slot{i}");
            _texts[i] = Scene.Find($"SlotText{i}");
            ApplyInk(_slots[i]);
        }
        _confirm = Scene.Find("SlotConfirm");
        _confirmYes = Scene.Find("SlotConfirmYes");
        _confirmNo = Scene.Find("SlotConfirmNo");
    }

    private static void ApplyInk(Entity e)
    {
        if (!e.IsValid) return;
        Ui.SetMaterial(e, "ui_ink_ui");
        Ui.SetMaterialColors(e, Vector4.Zero, InkEdge);
    }

    public void OnShown()
    {
        SaveSystem.EnsureLoaded();
        _pendingOverwrite = -1;
        if (_confirm.IsValid) _confirm.SetActive(false);
        RefreshLabels();
        // Focus the first selectable slot (in resume mode, the first USED slot).
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (_slots[i].IsValid && Selectable(i)) { Ui.SetFocus(_slots[i]); break; }
        }
    }

    private bool Selectable(int i) => !s_resumeOnly || SaveSystem.Slot(i).Exists;

    private void RefreshLabels()
    {
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (!_texts[i].IsValid) continue;
            SaveProfile p = SaveSystem.Slot(i);
            string label;
            if (!p.Exists) label = $"SLOT {Roman(i)}   empty - a new dark";
            else label = $"SLOT {Roman(i)}   {LevelName(p.FurthestLevel)} - {p.CompletedCount()}/4 done - {SlotCoins(p)} coins";
            Ui.SetText(_texts[i], label);
            Ui.SetInteractable(_slots[i], Selectable(i));
        }
    }

    private static int SlotCoins(SaveProfile p)
    {
        int c = 0;
        foreach (string k in SaveProfile.LevelKeys) c += p.Level(k).BestCoins;
        return c;
    }

    private static string Roman(int i) => i == 0 ? "I" : i == 1 ? "II" : "III";
    private static string LevelName(string key) => key switch
    {
        "Level1" => "1-1 The Cheerful Plunge",
        "Level2" => "1-2 Quiet, Please",
        "Level3" => "1-3 The Hollow Descent",
        "Level4" => "1-4 The Fourth Descent",
        _ => key,
    };

    public void HandleInput()
    {
        // Overwrite confirmation takes priority when open.
        if (_pendingOverwrite >= 0)
        {
            if (_confirmYes.IsValid && Ui.WasActivated(_confirmYes)) { StartFresh(_pendingOverwrite); }
            else if (_confirmNo.IsValid && Ui.WasActivated(_confirmNo)) { _pendingOverwrite = -1; if (_confirm.IsValid) _confirm.SetActive(false); OnShown(); }
            return;
        }

        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (!_slots[i].IsValid || !Selectable(i) || !Ui.WasActivated(_slots[i])) continue;
            SaveProfile p = SaveSystem.Slot(i);
            if (s_resumeOnly) { Resume(i); return; }
            if (p.Exists) { _pendingOverwrite = i; if (_confirm.IsValid) _confirm.SetActive(true); return; }
            StartFresh(i);
            return;
        }
    }

    private void StartFresh(int i)
    {
        SaveSystem.Wipe(i);
        SaveSystem.SetActive(i);
        GameState.TrialMode = false;
        GameState.ResumeLevel = "";
        GameState.ResumeCheckpoint = 0;
        Log.Info($"[INKBOUND] new dark in slot {i}");
        Scene.Load("Level1");
    }

    private void Resume(int i)
    {
        SaveSystem.SetActive(i);
        SaveProfile p = SaveSystem.Slot(i);
        GameState.TrialMode = false;
        GameState.ResumeLevel = p.FurthestLevel;
        GameState.ResumeCheckpoint = p.Level(p.FurthestLevel).FurthestCheckpoint;
        Log.Info($"[INKBOUND] resume slot {i} at {p.FurthestLevel} cp {GameState.ResumeCheckpoint}");
        Scene.Load(p.FurthestLevel);
    }

    public override void OnUpdate(float dt)
    {
        _t += dt;
        Vector4 mp = new(_t, 0f, 0f, 0f);
        for (int i = 0; i < SaveSystem.SlotCount; i++)
        {
            if (!_slots[i].IsValid) continue;
            Ui.SetMaterialParams(_slots[i], mp);
            bool on = Ui.IsFocused(_slots[i]);
            Vector4 col = Selectable(i) ? (on ? Cyan : Dim) : new Vector4(0.06f, 0.07f, 0.09f, 1f);
            if (on) col.W = 0.78f + 0.22f * MathF.Sin(_t * 4.2f);
            Ui.SetImageColor(_slots[i], col);
            if (_texts[i].IsValid) Ui.SetTextColor(_texts[i], on ? TextOn : (Selectable(i) ? TextMuted : new Vector4(0.3f, 0.32f, 0.38f, 1f)));
        }
    }
}
```

(Note: `Ui.SetInteractable` and `Ui.SetText`/`SetTextColor`/`SetImageColor`/`SetMaterial`/`SetMaterialColors`/`SetMaterialParams`/`IsFocused`/`WasActivated`/`SetFocus` are all existing SDK methods used by `LevelSelectScreen`; `Ui.SetInteractable` toggles a UISelectable.)

- [ ] **Step 3: Route Title `descend`/`return` into SlotSelect**

In `TitleScreen.HandleInput`, replace the `descend` and `return` handlers:

```csharp
        if (Activated(0)) { Log.Info("[INKBOUND] descend"); SlotSelectScreen.Open(resumeOnly: false); }
        else if (Activated(1)) { Log.Info("[INKBOUND] return"); SlotSelectScreen.Open(resumeOnly: true); }
        else if (Activated(2)) MenuController.Instance?.Go(MenuScreen.Settings);
        else if (Activated(3)) Log.Info("[INKBOUND] release (quit)");
        else if (_sandbox.IsValid && Ui.WasActivated(_sandbox)) { Log.Info("[INKBOUND] sandbox"); Scene.Load("Sandbox"); }
```

- [ ] **Step 4: Author the `SlotSelectRoot` subtree in `Menu.scene.toml`**

Mirror the `LevelSelectRoot` block (lines ~527-546) as the template. Create (in-editor via Hierarchy right-click UI, or hand-append + `rm` the `.scene.bin`) a disabled root `SlotSelectRoot` (full-screen UI Rect, script `SlotSelectScreen`, `parent` = the menu canvas entity index — same `parent`/`parent_node` as `LevelSelectRoot`), with children parented to it:

- `Slot0`, `Slot1`, `Slot2`: a UI Image + UI Rect (a wide row, e.g. `anchor_min/max` y at 0.62 / 0.50 / 0.38, x spanning `0.10..0.90`) + a `ui_selectable` with `group = 'slots'`, `interactable = true`.
- `SlotText0..2`: UI Text children (font `IBMPlexMono-Italic`, left-aligned, ASCII only), one per row, positioned over each slot.
- `SlotConfirm`: a small panel (UI Image + UI Rect, initially `disabled = true`) holding a UI Text "overwrite this dark? [y] descend anew  [n] keep it".
- `SlotConfirmYes`, `SlotConfirmNo`: two `ui_selectable` buttons (group `slots`) inside the confirm panel.

The entity **names** must match the `Scene.Find` strings exactly. Reuse the `ui_selectable` group pattern from `Node0` (lines ~589-591). Keep the root `disabled = true` (MenuController toggles it).

- [ ] **Step 5: Build + verify**

Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug` → `Build succeeded`.
Playtest: Play the Menu scene; activate `descend` -> the SlotSelect screen shows 3 rows ("empty - a new dark" the first run). Activate an empty slot -> `get_console_log` shows `new dark in slot 0` and Level1 loads. Return to menu, `return` -> only the used slot is focusable.
Optional screenshot: `screenshot` the SlotSelect screen to confirm styling reads as inky.

- [ ] **Step 6: Commit**

```bash
git add projects/INKBOUND/scripts/SlotSelectScreen.cs projects/INKBOUND/scripts/MenuController.cs projects/INKBOUND/scripts/TitleScreen.cs projects/INKBOUND/scenes/Menu.scene.toml
git commit -m "Add a 3-slot save-select screen behind descend and return"
```

---

## Task 5: Level Select reflects the active slot

**Files:**
- Modify: `projects/INKBOUND/scripts/LevelSelectScreen.cs`

**Interfaces:**
- Consumes: `SaveSystem.Active`, `SaveProfile.IsUnlocked`, `SaveProfile.Level(...)` (Tasks 1-2).
- Produces: per-active-slot lock/DONE/records driving the existing node styling. (No new external interface.)

- [ ] **Step 1: Derive node lock/DONE from the active profile**

Replace the hardcoded `Locked` reads. Add a helper and use it wherever `Nodes[i].Locked` is referenced (the placeholder nodes 4-5 stay hard-locked; real nodes 0-3 gate on the profile):

```csharp
    // A real level (index 0-3) is locked when the active slot has not unlocked it; the two
    // placeholder nodes (4-5) are always locked flavor.
    private static bool IsLocked(int i)
    {
        if (Nodes[i].Scene.Length == 0) return true;              // placeholder nodes
        SaveProfile? p = SaveSystem.Active;
        return p != null && !p.IsUnlocked(Nodes[i].Scene);
    }

    private static bool IsDone(int i)
    {
        SaveProfile? p = SaveSystem.Active;
        return Nodes[i].Scene.Length > 0 && p != null && p.Level(Nodes[i].Scene).Completed;
    }
```

In `HandleInput`, gate activation on `!IsLocked(i)` instead of `!Nodes[i].Locked`.
In `OnUpdate`, use `IsLocked(i)` for the locked colour branch, and set node interactable to `!IsLocked(i)`:

```csharp
            if (_nodes[i].IsValid) Ui.SetInteractable(_nodes[i], !IsLocked(i));
```

(Keep the existing focus/breathe styling; just swap the lock source.)

- [ ] **Step 2: Show records + DONE in the detail panel**

In `OnUpdate`, where the focused node mirrors into `_name/_stats/_flavor`, append records for real levels:

```csharp
        if (focused >= 0 && focused != _shown)
        {
            _shown = focused;
            SaveProfile? prof = SaveSystem.Active;
            string done = IsDone(focused) ? "  DONE" : "";
            if (_name.IsValid) Ui.SetText(_name, Nodes[focused].Name + done);
            string stats = Nodes[focused].Stats;
            if (prof != null && Nodes[focused].Scene.Length > 0)
            {
                LevelRecord r = prof.Level(Nodes[focused].Scene);
                string best = r.BestTimeSeconds == null ? "--:--" : FormatTime(r.BestTimeSeconds.Value);
                stats = $"BEST COINS {r.BestCoins}     TRIAL {best}";
            }
            if (_stats.IsValid) Ui.SetText(_stats, stats);
            if (_flavor.IsValid) Ui.SetText(_flavor, Nodes[focused].Flavor);
        }
```

Add the helper:

```csharp
    private static string FormatTime(float s)
    {
        int total = (int)s;
        return $"{total / 60:00}:{total % 60:00}";
    }
```

- [ ] **Step 3: Build + verify**

Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug` → `Build succeeded`.
Playtest: with a fresh slot active (start via SlotSelect), open Level Select -> only `Node0` (Level1) is focusable; nodes 1-3 read locked. This is verified fully in Task 7 after a completion unlocks the next node.

- [ ] **Step 4: Commit**

```bash
git add projects/INKBOUND/scripts/LevelSelectScreen.cs
git commit -m "Gate Level Select nodes on the active slot progress"
```

---

## Task 6: Time-trial toggle + HUD clock

**Files:**
- Modify: `projects/INKBOUND/scripts/LevelSelectScreen.cs`
- Modify: `projects/INKBOUND/scripts/HudController.cs`
- Modify: `projects/INKBOUND/assets/prefabs/GameHud.prefab.toml`

**Interfaces:**
- Consumes: `GameState.TrialMode`, `GameState.TrialElapsed`, `IsDone(i)` (Tasks 2, 5).
- Produces: pressing `T` on a completed, focused node toggles a per-node trial flag; descending it sets `GameState.TrialMode`. HUD shows the running clock when `TrialMode`.

- [ ] **Step 1: Add a trial toggle to `LevelSelectScreen.cs`**

Add a field and toggle handling in `HandleInput` (before the activation loop):

```csharp
    private bool _trialArmed;   // T on a completed focused node arms trial mode for the next descend
```

```csharp
    public void HandleInput()
    {
        int focused = FocusedIndex();
        if (focused >= 0 && IsDone(focused) && Input.IsKeyPressed(Key.T))
        {
            _trialArmed = !_trialArmed;
            Log.Info($"[INKBOUND] time trial {(_trialArmed ? "armed" : "off")} for {Nodes[focused].Scene}");
        }
        for (int i = 0; i < 6; i++)
        {
            if (!IsLocked(i) && _nodes[i].IsValid && Ui.WasActivated(_nodes[i]))
            {
                GameState.TrialMode = _trialArmed && IsDone(i);
                GameState.ResumeLevel = "";           // a trial/replay always starts from the level top
                GameState.ResumeCheckpoint = 0;
                Log.Info($"[INKBOUND] descend to {Nodes[i].Scene}{(GameState.TrialMode ? " [TRIAL]" : "")}");
                Scene.Load(Nodes[i].Scene);
            }
        }
    }

    private int FocusedIndex()
    {
        for (int i = 0; i < 6; i++) if (_nodes[i].IsValid && Ui.IsFocused(_nodes[i])) return i;
        return -1;
    }
```

Reset `_trialArmed = false;` in `OnShown()`. In the detail panel (Task 5 Step 2), when `_trialArmed && IsDone(focused)`, append `  > TRIAL ARMED (T)` to the stats string so the affordance is visible.

- [ ] **Step 2: Add a clock element to the HUD prefab**

In `GameHud.prefab.toml`, add a UI Text child `HudTrialClock` (font `PixelStorm`, top-centre, ASCII), initially empty. Mirror the existing HUD counter entity's structure for anchors/parenting.

- [ ] **Step 3: Drive the clock from `HudController.cs`**

In `HudController`, resolve the clock in `OnAttach` (`Scene.Find("HudTrialClock")`) and update it each frame:

```csharp
        if (_clock.IsValid)
        {
            if (GameState.TrialMode)
            {
                int total = (int)GameState.TrialElapsed;
                Ui.SetText(_clock, $"{total / 60:00}:{total % 60:00}.{(int)((GameState.TrialElapsed - total) * 100):00}");
            }
            else Ui.SetText(_clock, "");
        }
```

(Add the `_clock` field and its `Scene.Find` in `OnAttach`.)

- [ ] **Step 4: Build + verify**

Run: `dotnet build projects/INKBOUND/scripts/AetherGame.csproj -c Debug` → `Build succeeded`.
Playtest (after a completion so a node is DONE): focus that node, press `T` -> `get_console_log` shows `time trial armed`; descend -> HUD shows a running clock; reach the goal -> the profile's `BestTimeSeconds` is set (checked in Task 7).

- [ ] **Step 5: Commit**

```bash
git add projects/INKBOUND/scripts/LevelSelectScreen.cs projects/INKBOUND/scripts/HudController.cs projects/INKBOUND/assets/prefabs/GameHud.prefab.toml
git commit -m "Add per-level time-trial mode with a HUD clock"
```

---

## Task 7: End-to-end verification pass

**Files:** none (verification + fixes only).

- [ ] **Step 1: Fresh-profile gauntlet via MCP**

With the editor running INKBOUND on the Menu scene, delete any existing `%LocalAppData%/AetherCore/INKBOUND/slot*.json`, then Play and drive:
1. `descend` -> pick empty SLOT I -> Level1 loads.
2. Reach a checkpoint (`get_console_log`: `Checkpoint 1 reached!`); confirm `slot0.json` now exists with `FurthestLevel = "Level1"` and `Level1.FurthestCheckpoint >= 1`.
3. Reach the goal (`get_console_log`: `Level complete`); confirm `slot0.json` has `Level1.Completed = true`.
4. Back to menu -> Level Select -> `Node1` (Level2) is now focusable/unlocked; `Node0` shows `DONE`.

- [ ] **Step 2: Resume gauntlet**

Quit to menu (or restart the editor to prove disk persistence), Play, `return` -> SLOT I is the only focusable slot -> selecting it loads `FurthestLevel` and the player spawns at the recorded checkpoint (not the level start). Confirm via player position / a `screenshot`.

- [ ] **Step 3: Overwrite + trial gauntlet**

`descend` onto the used SLOT I -> confirm prompt -> choose "keep it" (`[n]`) -> returns to slot list unchanged; choose `descend` again -> `[y]` -> `slot0.json` resets (`CompletedCount()==0`). Then, on a completed level, arm trial (`T`) and descend -> HUD clock runs -> finish -> `slot0.json` `Level1.BestTimeSeconds` is set; re-run slower -> best time does NOT increase.

- [ ] **Step 4: Regression sweep**

Run the engine gauntlet if applicable (`run_gauntlet` MCP) and confirm 0 script errors in `get_console_log` across a Title -> SlotSelect -> Level -> dialogue -> win loop. Fix any issue found, committing each fix with a plain imperative message.

- [ ] **Step 5: Finish**

Per project convention, fast-forward merge to `master` and push (use superpowers:finishing-a-development-branch if working on a branch).

---

## Self-Review

**Spec coverage:**
- Persistence pattern (LocalAppData/JSON, settings stay global) → Task 1.
- Per-level data (Completed, BestCoins, BestTimeSeconds, FurthestCheckpoint) → Task 1 `LevelRecord`.
- Derived linear unlock + permanent placeholder locks → Task 1 `IsUnlocked` + Task 5 `IsLocked`.
- Completion recording (coins max, unlock next, reset checkpoint) → Task 2.
- Checkpoint record + index + resume-at-checkpoint → Task 3.
- Multiple slots + shared SlotSelect screen + descend(new/overwrite)/return(resume) → Task 4.
- Level Select DONE/best-coins/best-time + lock from slot → Task 5.
- No story timer; time-trial toggle on completed node + HUD clock + best-time only from trials → Tasks 2 (record), 6 (toggle/clock).
- Level self-identification (no engine current-scene API) → Task 2 `LevelInfo`.
- Out-of-scope (audio, crystal tally) → untouched.

**Placeholder scan:** All code steps carry full source; scene-authoring steps name exact entity names and reference the concrete template block (LevelSelectRoot / Node0). No "TBD"/"add error handling"/"similar to Task N".

**Type consistency:** `SaveProfile`/`LevelRecord`/`SaveSystem` member names and signatures are defined in Task 1 and used verbatim in Tasks 2-6. `GameState.CurrentLevel/TrialMode/TrialElapsed/ResumeLevel/ResumeCheckpoint/ConsumeResume` defined in Tasks 2-3 and consumed consistently. `Checkpoint.Index`, `PlayerController.TeleportTo`, `MenuScreen.SlotSelect`, `SlotSelectScreen.Open` all match across tasks.
