# Whisper Testbed Game Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A standalone AetherCore project, **Whisper**, in which four or more players connect by IP, run around a shared arena seeing each other move with names above their heads, and chat.

**Architecture:** Whisper is a consumer of the replication framework, not a second implementation of it. The host runs the authoritative simulation; each client predicts its own player locally and eases toward the host's correction. Remote players are interpolated from the snapshot buffer. Everything project-specific — the connect screen, chat, name tags, spawn policy — lives in `projects/Whisper/scripts/`.

**Tech Stack:** C#/CoreCLR game scripts, AetherCore SDK, the Whisper replication framework, Box2D 2D physics, the tilemap renderer.

**Spec:** `docs/superpowers/specs/2026-07-27-whisper-multiplayer-design.md`

**This is plan 2 of 2.** It requires `docs/superpowers/plans/2026-07-27-whisper-framework.md` to be complete — every task here consumes the `Net` API it produces.

## Global Constraints

- **No game code in `src/engine` or `src/app`.** Everything in this plan is either a project asset, a project C# script, or a project scene. If a task seems to need an engine change, that is a signal the framework is missing something — stop and report it rather than putting game logic in the engine.
- **The host is authoritative.** A client never writes another player's state. Clients own exactly one entity: their own player.
- **Prediction corrects by easing, never by rewinding.** The local player runs the real `PlayerController` against a real Box2D body.
- Player name, position and animation state reach other players **only** through replication or RPC — never through a side channel.
- Project layout mirrors INKBOUND: `assets/`, `scenes/`, `scripts/`, `ProjectSettings.toml`.
- Scripts are C# in namespace `AetherGame`, 4-space indent, XML doc comments on public members, `EntityScript` subclasses.
- Commit style: plain imperative subject, optional flat bullet body. NO `feat:`/`fix:` prefixes, no scopes, no emoji, no attribution trailers.
- Build tree: `build/ninja-clang`. **Close the editor before rebuilding** — a running editor locks `build/ninja-clang/data/scripts/managed` and the staging copy fails with "Permission denied", silently leaving targets unrelinked.
- The editor must be launched with `--project D:/AetherCore/projects/Whisper`, from CWD `build/ninja-clang`, with `src/app/RelWithDebInfo` on `PATH` (it holds `nethost.dll`), and `AETHER_CONTROL_PORT=8787` set for MCP.

## File Structure

**Created — project scaffold**
- `projects/Whisper/ProjectSettings.toml` — modelled on INKBOUND's, `startupscene = "Title"`.
- `projects/Whisper/scripts/AetherGame.csproj` — copied from INKBOUND, same relative `ProjectReference`.

**Created — borrowed assets**
- `projects/Whisper/assets/textures/player/` — from INKBOUND.
- `projects/Whisper/assets/textures/world/tileset_16x16.png` + `.spriteatlas.toml`.
- `projects/Whisper/assets/tilemaps/arena.tilemap` + `arena.tileset.toml`.
- `projects/Whisper/assets/prefabs/player.prefab.toml`.

**Created — scenes**
- `projects/Whisper/scenes/Title.scene.toml` — connect screen.
- `projects/Whisper/scenes/Arena.scene.toml` — the play space.

**Created — scripts**
- `PlayerController.cs` — lifted from INKBOUND, then gated on ownership.
- `NetPlayerSync.cs` — prediction/correction and animation replication for one player.
- `ConnectScreen.cs` — host/join UI.
- `ChatBox.cs` — chat input, log, and the RPC.
- `NameTag.cs` — world-to-screen name label.
- `WhisperSession.cs` — session lifecycle: spawn on join, despawn on leave, host-quit handling.

## Task 1: Project scaffold

**Files:**
- Create: `projects/Whisper/ProjectSettings.toml`, `projects/Whisper/scripts/AetherGame.csproj`, `projects/Whisper/scenes/`, `projects/Whisper/assets/`

**Interfaces:**
- Consumes: nothing.
- Produces: a project the editor can open.

- [ ] **Step 1: Create the settings file**

Create `projects/Whisper/ProjectSettings.toml`:

```toml
# AetherCore project file.

[app]
startupscene = "Title"
targetfps = 0

[graphics]
asynccompute = true
fxaa = false
vsync = false

[paths]
assets = "assets"
prefabs = "assets/prefabs"
scenes = "scenes"
scripts = "scripts"

[project]
name = "Whisper"
version = 1
```

No `[cursor]` block: Whisper uses the OS pointer. INKBOUND's custom cursor is tied to its ink mechanic and is exactly the kind of thing not to copy.

- [ ] **Step 2: Create the csproj**

Copy `projects/INKBOUND/scripts/AetherGame.csproj` to `projects/Whisper/scripts/AetherGame.csproj` unchanged. The `ProjectReference` path `../../../managed/AetherCore/AetherCore.csproj` is correct at the same depth, and `AssemblyName`/`RootNamespace` stay `AetherGame` — the engine loads that assembly name by convention.

- [ ] **Step 3: Create the directory skeleton**

```bash
mkdir -p projects/Whisper/assets/textures/player projects/Whisper/assets/textures/world projects/Whisper/assets/tilemaps projects/Whisper/assets/prefabs projects/Whisper/scenes
```

- [ ] **Step 4: Verify the editor opens it**

Close any running editor, then:

```bash
cd build/ninja-clang && PATH="$PWD/src/app/RelWithDebInfo:$PATH" AETHER_CONTROL_PORT=8787 ./src/app/Editor.exe --project D:/AetherCore/projects/Whisper
```

Expected: the editor opens with project "Whisper" in the status bar. It will report a missing startup scene — that is expected until Task 3.

- [ ] **Step 5: Commit**

```bash
git add projects/Whisper
git commit -m "Scaffold the Whisper project"
```

---

## Task 2: Borrow the player assets and controller

**Files:**
- Create: `projects/Whisper/assets/textures/player/*` (copied)
- Create: `projects/Whisper/scripts/PlayerController.cs` (copied)
- Create: `projects/Whisper/assets/prefabs/player.prefab.toml`

**Interfaces:**
- Consumes: nothing from the framework yet.
- Produces: a `player` prefab with a sprite, a 2D dynamic body, and `PlayerController`.

- [ ] **Step 1: Copy the sprites**

```bash
cp -r projects/INKBOUND/assets/textures/player/* projects/Whisper/assets/textures/player/
ls projects/Whisper/assets/textures/player/
```

Copy any `.spriteatlas.toml` alongside the PNGs — the animation system resolves frames through it.

- [ ] **Step 2: Copy the controller**

```bash
cp projects/INKBOUND/scripts/PlayerController.cs projects/Whisper/scripts/PlayerController.cs
```

It references no other INKBOUND script, so it compiles as-is. **Read it before continuing** and delete any field or branch that refers to INKBOUND-only concepts (checkpoints, ink, death-melt) — leave movement, jump, coyote time, jump buffer, drop-through and animation switching. Report what you removed.

- [ ] **Step 3: Build the player prefab**

With the editor open on Whisper, create an entity with a sprite renderer pointing at the idle player texture, a `Physics2D` dynamic body with a box collider sized to the sprite, a `SpriteAnimator`, and the `PlayerController` script. Save it as `projects/Whisper/assets/prefabs/player.prefab.toml`.

Do **not** add `NetworkIdentity` yet — Task 5 does that, so this task's deliverable is independently reviewable as "a working single-player character".

- [ ] **Step 4: Verify it moves**

Create a scratch scene with the prefab and some ground, press Play, and confirm the character runs and jumps. Screenshot it.

- [ ] **Step 5: Commit**

```bash
git add projects/Whisper
git commit -m "Add the Whisper player prefab and controller"
```

---

## Task 3: The arena

**Files:**
- Create: `projects/Whisper/assets/textures/world/tileset_16x16.png` + `.spriteatlas.toml` (copied)
- Create: `projects/Whisper/assets/tilemaps/arena.tilemap`, `arena.tileset.toml`
- Create: `projects/Whisper/scenes/Arena.scene.toml`

**Interfaces:**
- Consumes: nothing.
- Produces: `Arena` scene with collidable geometry, a 2D camera, and four named spawn-point entities `Spawn0`..`Spawn3`.

- [ ] **Step 1: Copy the tileset**

```bash
cp projects/INKBOUND/assets/textures/world/tileset_16x16.png projects/Whisper/assets/textures/world/
cp projects/INKBOUND/assets/textures/world/tileset_16x16.spriteatlas.toml projects/Whisper/assets/textures/world/
```

- [ ] **Step 2: Paint the arena**

In the editor: `new_scene` with `kind=2d`, add a tile layer using the copied tileset, and paint a compact arena — a solid floor, two or three platforms at jumpable heights, and walls at both ends so players cannot leave the play space. Keep it small enough that all four players are visible at once; this is a testbed, not a level.

Enable collision on the tile layer so the physics bodies collide with it.

- [ ] **Step 3: Add spawn points and camera**

Create four empty entities named exactly `Spawn0`, `Spawn1`, `Spawn2`, `Spawn3`, spaced along the floor. Add a main camera framing the arena.

- [ ] **Step 4: Save and verify**

Save as `Arena`. Drop the player prefab in and press Play: the character must land on the tiles and collide with the walls. Screenshot.

- [ ] **Step 5: Commit**

```bash
git add projects/Whisper
git commit -m "Add the Whisper arena scene"
```

---

## Task 4: Connect screen

**Files:**
- Create: `projects/Whisper/scenes/Title.scene.toml`
- Create: `projects/Whisper/scripts/ConnectScreen.cs`
- Create: `projects/Whisper/scripts/WhisperSession.cs` (the static name only; Task 5 fills in the rest)

**Interfaces:**
- Consumes: `Net.Host(port)`, `Net.Connect(ip, port)`, `Net.LastError`, `Net.IsConnected` (framework Task 11); `Ui.*` text box API.
- Produces: `WhisperSession.LocalPlayerName` (static string, set here and read by Task 5); a Title scene that transitions to `Arena` once hosting or connected.

**Create `WhisperSession.cs` in this task, not Task 5.** `ConnectScreen` writes the chosen name before the arena loads, so the field has to exist now. Task 5 adds the spawn and lifecycle logic to the same class:

```csharp
namespace AetherGame;

/// <summary>
/// Session-wide state that outlives a scene load. The name is chosen on the title
/// screen but is not needed until the arena spawns the player, so it lives here
/// rather than being threaded through the scene transition.
/// </summary>
public static partial class WhisperSession
{
    /// <summary>Name this player typed on the connect screen; "Player" if blank.</summary>
    public static string LocalPlayerName = "Player";
}
```

Declared `partial` so Task 5 can add the `EntityScript` half in the same file without rewriting this one. If the project's C# style disallows partial statics across a file, merge the two in Task 5 instead and note it.

- [ ] **Step 1: Build the UI**

In the Title scene, create a canvas with:
- A `UI Text Box` named `NameField`, `placeholder = "Your name"`, `max_length = 16`, `content_type = alphanumeric`.
- A `UI Text Box` named `AddressField`, `placeholder = "127.0.0.1:7777"`, `content_type = host`, `max_length = 21`.
- A `UI Button` `HostButton` labelled "Host".
- A `UI Button` `JoinButton` labelled "Join".
- A `UI Text` `StatusText`, empty.

- [ ] **Step 2: Write the script**

Create `projects/Whisper/scripts/ConnectScreen.cs`:

```csharp
using AetherCore;

namespace AetherGame;

/// <summary>
/// Title screen: choose a name, then host a session or join one by address.
/// Loads the arena once the transport reports success. This script is the only
/// place that decides a port default, so the address field can stay optional.
/// </summary>
public sealed class ConnectScreen : EntityScript
{
    public UiTextBoxRef NameField;
    public UiTextBoxRef AddressField;
    public Entity HostButton;
    public Entity JoinButton;
    public Entity StatusText;

    public ushort DefaultPort = 7777;

    public override void OnUpdate()
    {
        if (Ui.WasClicked(HostButton))
        {
            StartHost();
        }
        else if (Ui.WasClicked(JoinButton) || Ui.WasSubmitted(AddressField.Entity))
        {
            StartJoin();
        }
    }

    private void StartHost()
    {
        RememberName();
        if (Net.Host(DefaultPort))
        {
            Scene.Load("Arena");
        }
        else
        {
            Ui.SetText(StatusText, $"Could not host: {Net.LastError}");
        }
    }

    private void StartJoin()
    {
        RememberName();
        (string ip, ushort port) = ParseAddress(Ui.GetTextBoxText(AddressField.Entity));
        if (Net.Connect(ip, port))
        {
            Ui.SetText(StatusText, $"Connecting to {ip}:{port}...");
            Scene.Load("Arena");
        }
        else
        {
            Ui.SetText(StatusText, $"Could not connect: {Net.LastError}");
        }
    }

    private void RememberName()
    {
        string name = Ui.GetTextBoxText(NameField.Entity).Trim();
        WhisperSession.LocalPlayerName = string.IsNullOrEmpty(name) ? "Player" : name;
    }

    /// <summary>Split "host" or "host:port"; an absent or unparseable port falls back
    /// to the default rather than refusing to connect.</summary>
    private (string, ushort) ParseAddress(string text)
    {
        string trimmed = text.Trim();
        if (trimmed.Length == 0)
        {
            return ("127.0.0.1", DefaultPort);
        }
        int colon = trimmed.LastIndexOf(':');
        if (colon <= 0 || colon == trimmed.Length - 1)
        {
            return (trimmed, DefaultPort);
        }
        string host = trimmed.Substring(0, colon);
        return ushort.TryParse(trimmed.Substring(colon + 1), out ushort port)
            ? (host, port)
            : (host, DefaultPort);
    }
}
```

If `UiTextBoxRef` does not exist as a component-ref wrapper, use a plain `Entity` field for each text box and call `Ui.GetTextBoxText(entity)` directly — check `managed/AetherCore/ComponentRef.cs` for the available wrappers and report which route you took.

- [ ] **Step 3: Wire and verify**

Assign the script's fields in the inspector, set `startupscene = "Title"`, and Play. Confirm typing works in both fields, that Host loads the arena, and that Join with an unreachable address shows an error rather than hanging.

- [ ] **Step 4: Commit**

```bash
git add projects/Whisper
git commit -m "Add the Whisper connect screen"
```

---

## Task 5: Replicated players

**Files:**
- Modify: `projects/Whisper/assets/prefabs/player.prefab.toml`
- Create: `projects/Whisper/scripts/WhisperSession.cs`
- Modify: `projects/Whisper/scripts/PlayerController.cs`

**Interfaces:**
- Consumes: `Net.Spawn`, `Net.Despawn`, `Net.IsHost`, `Net.IsOwner`, `Net.HasAuthority`, `Net.SetPlayerName`, `Net.LocalConnectionId` (framework Task 11); `NetworkIdentity`, `NetworkTransform`, `NetPlayer` components (framework Task 4).
- Produces: `WhisperSession.LocalPlayerName` (static string); one replicated player entity per connection.

- [ ] **Step 1: Add the network components to the prefab**

Add `Network Identity`, `Network Transform` and `Net Player` to the player prefab. Leave `Network Identity`'s runtime fields alone — the host assigns them.

- [ ] **Step 2: Write the session script**

Create `projects/Whisper/scripts/WhisperSession.cs`. Place it on a single entity in the Arena scene. Responsibilities:

- Hold `public static string LocalPlayerName` (set by `ConnectScreen`).
- On the **host**: spawn a `player` prefab at `Spawn{n}` for the host itself on scene start, and for each joining connection; despawn on disconnect. Assign the spawn index round-robin from the connection count.
- On a **client**: send its chosen name to the host once connected, via a `[NetRpc(NetRpcTarget.Server)]` method that sets `NetPlayer.displayName` on that connection's player entity. Because `displayName` is `AE_FIELD_REP`, the host's write replicates to everyone with no extra code.
- On **host disconnect** (client side): return to `Title` with a message.

- [ ] **Step 3: Gate input on ownership**

In `PlayerController.OnUpdate`, take input only for the owned player:

```csharp
        // Every client runs this script on every player entity, including other
        // people's. Only the owner reads input; the rest are driven by replication.
        if (!Net.IsOwner(Self))
        {
            return;
        }
```

Place this as the first statement of `OnUpdate`, before any input read. Physics still simulates all bodies on the host — only *input* is owner-gated.

- [ ] **Step 4: Verify two instances**

Build `GameRuntime`, publish Whisper, launch one instance as host and one as client, and confirm two characters appear and each moves only under its own player's input. Screenshot both windows.

- [ ] **Step 5: Commit**

```bash
git add projects/Whisper
git commit -m "Spawn and replicate Whisper players"
```

---

## Task 6: Prediction and correction

**Files:**
- Create: `projects/Whisper/scripts/NetPlayerSync.cs`

**Interfaces:**
- Consumes: `Net.IsOwner`, `Net.IsHost`; `NetworkTransform` settings (framework Task 4); the framework's eased correction.
- Produces: visually smooth local and remote motion.

- [ ] **Step 1: Write the sync script**

Create `projects/Whisper/scripts/NetPlayerSync.cs`, attached to the player prefab. It:

- On the **owned** entity: does nothing to the transform. `PlayerController` drives it, and the framework's receive system eases toward the host's authoritative position. This script only tunes `NetworkTransform.correctionRate` and `snapDistance` from inspector fields, so the feel is adjustable without a rebuild.
- On **remote** entities: disables the local `PlayerController` (so two simulations do not fight) and lets the framework's interpolation drive the transform.
- On **all** entities: mirrors the animation state so remote players animate. The clean way is to mark the animator's current-clip field `AE_FIELD_REP` in the engine — but that is an engine change, so instead expose a `[Replicated] public int AnimState;` on this script, set it on the host from the controller's state, and switch the sprite animation from it on every client.

`AnimState` is exactly the case replicated script fields exist for: game state, owned by the game, replicated without touching the engine.

- [ ] **Step 2: Verify**

With two instances connected, confirm the remote character animates between idle/run/jump and that its motion is smooth rather than stepping between packets. Record a short clip or two screenshots a second apart.

- [ ] **Step 3: Commit**

```bash
git add projects/Whisper
git commit -m "Predict the local Whisper player and interpolate remote ones"
```

---

## Task 7: Name tags

**Files:**
- Create: `projects/Whisper/scripts/NameTag.cs`

**Interfaces:**
- Consumes: `Camera.WorldToScreen` (framework Task 11); `NetPlayer.displayName` (replicated); `Ui.CreateText`, `Ui.SetRect`, `Ui.SetText`.
- Produces: a canvas label tracking each player.

- [ ] **Step 1: Write the script**

Create `projects/Whisper/scripts/NameTag.cs`, attached to the player prefab. On attach it creates a `UI Text` under the HUD canvas; each frame it projects the player's world position plus a vertical offset through `Camera.WorldToScreen` and positions the label there, reading its string from the replicated `NetPlayer.displayName`.

It must hide the label when `WorldToScreen` returns `(-1, -1)` (behind the camera) and destroy the label in `OnDetach`, or a despawned player leaves an orphaned tag floating in the HUD.

- [ ] **Step 2: Verify**

Two instances, two different names typed on the title screen: each character must carry the other's name, and the tag must follow the character as it moves and jumps. Screenshot.

- [ ] **Step 3: Commit**

```bash
git add projects/Whisper
git commit -m "Draw player names above Whisper characters"
```

---

## Task 8: Chat

**Files:**
- Create: `projects/Whisper/scripts/ChatBox.cs`
- Modify: `projects/Whisper/scenes/Arena.scene.toml`

**Interfaces:**
- Consumes: `[NetRpc(NetRpcTarget.Server)]` and `[NetRpc(NetRpcTarget.Multicast)]` (framework Task 10); `NetPlayer.displayName`; the `UI Text Box` widget.
- Produces: chat visible to every connected player.

- [ ] **Step 1: Build the chat UI**

In the Arena scene, add to the HUD canvas a `UI Text Box` named `ChatInput` anchored bottom-left (`placeholder = "Press Enter to chat"`, `max_length = 120`, `content_type = any`), and a `UI Text` `ChatLog` above it, left-aligned, wrapping, sized for about eight lines.

- [ ] **Step 2: Write the script**

Create `projects/Whisper/scripts/ChatBox.cs`:

- Enter with the box not editing calls `Ui.BeginEdit(ChatInput)`; Enter while editing submits.
- On submit, if the text is non-empty, call a `[NetRpc(NetRpcTarget.Server)]` method with the message.
- The host's handler prefixes the sender's replicated `displayName` and calls a `[NetRpc(NetRpcTarget.Multicast)]` method that appends the line to every client's log.
- The log keeps the last N lines (8) and joins them with `\n` into `ChatLog`.
- **While the chat box is editing, the player must not move**: `PlayerController` already returns early for non-owners; add a check so it also returns while `Ui.IsEditing(ChatInput)` is true. Otherwise typing "d" walks the character right.

That last point is the kind of thing only found by playing it — it is called out here so it is built in rather than discovered.

- [ ] **Step 3: Verify**

Two instances: type a message on each, and confirm both see both lines attributed to the right names, and that typing does not move the character.

- [ ] **Step 4: Commit**

```bash
git add projects/Whisper
git commit -m "Add in-game chat to Whisper"
```

---

## Task 9: Session lifecycle

**Files:**
- Modify: `projects/Whisper/scripts/WhisperSession.cs`, `ChatBox.cs`

**Interfaces:**
- Consumes: framework connect/disconnect events surfaced through `Net`.
- Produces: clean join/leave behaviour.

- [ ] **Step 1: Implement the cases**

- **Client joins:** host spawns their player; every client sees it appear; a chat line announces "`<name>` joined".
- **Client leaves:** host despawns their player and their name tag disappears everywhere; a chat line announces "`<name>` left".
- **Host quits:** every client tears down replicated entities and returns to `Title` with "Host disconnected" in the status text. No host migration — this is a stated non-goal.

- [ ] **Step 2: Verify each case**

Run three instances. Kill one client (close its window) and confirm the other two see it vanish with a chat line. Then kill the host and confirm both clients return to the title with the message rather than freezing or crashing. Screenshot each.

- [ ] **Step 3: Commit**

```bash
git add projects/Whisper
git commit -m "Handle Whisper join, leave and host quit"
```

---

## Task 10: Four-player verification

No production code. Nothing here is complete until these produce output.

- [ ] **Step 1: Full build**

Close the editor first.

```bash
cmake --build build/ninja-clang --target CompileShaders EngineAssetsPak Editor GameRuntime EngineTests
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all targets succeed, suite green.

- [ ] **Step 2: Four participants**

Host from editor Play (so the inspector and console are available on the host), then launch three standalone instances joining `127.0.0.1:7777`, each with a distinct name.

- [ ] **Step 3: Confirm all four goals against the spec's acceptance criteria**

1. Each client entered an address to join; the editor hosted. ✅ when all three connect.
2. Four concurrent players, all visible.
3. A chat conversation with every participant sending and all four seeing every line correctly attributed.
4. All four characters moving simultaneously, each with the right name tracking it.

Screenshot the host's view with all four players and the chat log visible.

- [ ] **Step 4: Record what the testbed cannot show**

Relevancy filtering is active but unobservable at this scale — all four players are always within radius of each other. State that plainly in the report rather than claiming it verified.

- [ ] **Step 5: Merge**

```bash
git status
```

If clean, fast-forward merge to master and push, per the project's standing practice.

---

## Verification Summary

```bash
cmake --build build/ninja-clang --target CompileShaders EngineAssetsPak Editor GameRuntime EngineTests && ./build/ninja-clang/tests/EngineTests.exe
```

The framework's correctness is proven by plan 1's unit and loopback tests. This plan's proof is four processes on screen at once — do not report a task complete without the screenshot or console output for its verification step.
