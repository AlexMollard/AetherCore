# Getting started

Building a small 2D game in AetherCore, end to end: a sprite you can move, something
to collide with, a second scene, and a package you can hand to someone.

Everything below was run against the editor rather than written from memory. If a step
does not behave as described, that is a bug worth reporting.

## Before you start

You need the repo built once. From the checkout root:

```powershell
cmake --preset default
cmake --build --preset default
```

That produces the Launcher, the Editor, the standalone runtime and the asset tools under
`build/default/`. The [README](../README.md#quick-start) covers requirements (Visual Studio,
Vulkan SDK, .NET 10) and the other presets.

## 1. Make a project

Start the Launcher. **Run it from the build-tree root** (`build/default/` below) — `shaders://` resolves
relative to the working directory, and starting it from anywhere else fails at pipeline
creation.

```powershell
cd build/default
./src/app/RelWithDebInfo/Launcher.exe
```

Choose **Blank 2D**, name it, pick a folder. The Launcher spawns an Editor for that project
and closes itself.

With the default C# language, you get an orthographic camera, a `scripts/` folder with
a starter `Player.cs`, and a Player with that script already attached. The project folder
is yours — scenes, assets, prefabs and scripts all live in it, and
`ProjectSettings.toml` at its root *is* the project.

## 2. Put something on screen

Select the starter Player and look at the **Inspector**. To make another object, use
**Scene > + > Sprite**:
a `Sprite Renderer` with no texture renders as a white quad, which is enough to see it move.

To use real art, drop a PNG into the project's `assets/` folder — the **File Explorer** panel
picks it up — then set it as the sprite's texture. For a sprite sheet, open the **Sprite
Slicer** and cut regions non-destructively; the slices keep stable identities if you re-cut later.

## 3. Make it move

`scripts/Player.cs` already exists. Open it (the **Build** panel's **Debug C#** button opens
the solution in Visual Studio with the debugger attached, or edit it in any editor):

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

public sealed class Player : EntityScript
{
    public float Speed = 5.0f;

    public override void OnUpdate(float deltaTime)
    {
        float x = Input.GetAxisRaw(Key.A, Key.D) + Input.GetAxisRaw(Key.Left, Key.Right);
        float y = Input.GetAxisRaw(Key.S, Key.W) + Input.GetAxisRaw(Key.Down, Key.Up);
        Self.Position += new Vector3(x, y, 0.0f) * Speed * deltaTime;
    }
}
```

The starter Player already has this script attached. For a new sprite, use
**Add Component → Script → Player** to give it the same behaviour. `Speed` appears in the
Inspector because it is a public field; edit it there and the value saves with the scene.

Press **Play**. The editor builds the scripts first — a compile error stops Play and shows the
file, line and error in the Console. While Play is running, **F5** rebuilds and hot-reloads your
code without leaving Play.

`Self` is the entity the script is attached to. The lifecycle is `OnAttach` on the first tick,
`OnUpdate` every tick after, `OnDetach` when the entity or component goes away.

## 4. Add physics

Give the player a body and something to stand on.

Select the player and **Add Component → Rigid Body 2D**, then **Collider 2D**. Set the body type
to `Dynamic`. Create a second entity for the ground, give it a `Collider 2D` and a `Rigid Body 2D`
set to `Static`, and scale it wide.

Drive a dynamic body through velocities and forces, not by writing `Self.Position` — writing the
transform fights the simulation:

```csharp
public override void OnUpdate(float deltaTime)
{
    float x = Input.GetAxisRaw(Key.A, Key.D);
    Vector2 v = Physics2D.GetLinearVelocity(Self);
    Physics2D.SetLinearVelocity(Self, new Vector2(x * Speed, v.Y));

    if (Input.IsKeyPressed(Key.Space))
    {
        Physics2D.AddImpulse(Self, new Vector2(0.0f, JumpImpulse));
    }
}
```

> A **kinematic** body is the exception: those are transform-driven, so you move them by writing
> `Self.Position` each frame and *not* by setting velocity.

Collision callbacks are overrides — no polling, no registration. Override one and the runtime
turns events on for that entity:

```csharp
public override void OnCollisionEnter2D(Entity other)
{
    Log.Info($"hit {other.Name}");
}

public override void OnTriggerEnter2D(Entity other) { }
```

Set a collider to **trigger** in the Inspector to get the trigger pair instead.

## 5. A second scene

Scenes are files under the project's `scenes/`. **File → New Scene** makes one; save it as
`Level2`.

Switch at runtime by name:

```csharp
if (reachedTheExit)
{
    Scene.Load("Level2");
}
```

The current scene tears down at the end of the frame's script update, never mid-callback, so code
after the call still runs. Stopping Play in the editor restores whatever scene you were editing.

To carry an entity across the switch, mark it:

```csharp
Self.DontDestroyOnLoad();
```

Which scene the game boots is the **Startup Scene** dropdown in the **Project** panel. Set it
before publishing — a published game has no editor to fall back on.

## 6. Publish

**Build panel → Publish.** One button, no options: the destination is derived from the project,
and everything is baked in — assets packed into `project.pak`, scripts compiled, startup scene and
autoplay written into the package's own settings.

Output lands in `<project>/Builds/<platform>/<product>/`, containing the runtime executable, the
paks and the managed assemblies. Hand someone that folder and it runs.

Publishing uses **the editor's own build configuration** — a Debug editor publishes a Debug game.
For anything you intend to hand out, build the editor with `vs2022-msvc-release` (Ship) or
`vs2022-msvc-retail` (Retail) and publish from that.

You can also drive it without the UI, which is what the test tooling does:

```powershell
./build/default/tools/control-client/RelWithDebInfo/aether-ctl.exe project.publish
./build/default/tools/control-client/RelWithDebInfo/aether-ctl.exe project.publish_status
```

## Sharing a project with someone else

A generated `scripts/AetherGame.csproj` references the engine SDK by absolute path, because a
project can live anywhere on disk. That path is only the default — if your engine checkout is
somewhere else than the machine the project was created on, set:

```powershell
$env:AETHERCORE_SDK = "C:/your/checkout/managed/AetherCore/AetherCore.csproj"
```

The build reports a clear error naming this variable if the baked path does not exist.

## Controllers

`Gamepad` works the same way `Input` does, and needs no setup — plug a controller in and it is
there. Any pad the engine recognises reports through one fixed layout, so you never branch on
hardware:

```csharp
float x = Input.GetAxisRaw(Key.A, Key.D) + Gamepad.LeftStick.X;

if (Input.IsKeyPressed(Key.Space) || Gamepad.IsPressed(GamepadButton.A))
{
    Physics2D.AddImpulse(Self, new Vector2(0.0f, JumpImpulse));
}
```

Reading a pad that is not plugged in gives zero and `false`, so the code above is safe with no
controller attached — there is nothing to guard.

Three things are handled for you, and each is a bug you would otherwise have to find yourself:

- **You never pass a slot number.** Controller slots are not packed, so a lone pad often is not
  slot 0 — a wireless receiver or a virtual device can hold it. Every call defaults to *the first
  connected pad*. Pass a slot only for local multiplayer, where it identifies the player.
- **`Y` is up, and a diagonal is not faster.** Controllers report sticks upside down relative to
  world space, and a naive deadzone lets a diagonal push read 41% faster than a straight one.
- **A released trigger is `0`.** Controllers rest triggers at `-1`, not zero. `Gamepad.LeftTrigger`
  runs 0 to 1 like you would expect.

`Gamepad.GetAxisRaw` gives you the untouched value if you want to build your own response curve.

**Menus need no controller code at all.** Anything navigable - a scene-authored `UI Button`, or
any element you call `Ui.SetSelectable` on - is driven by the engine's UI navigation: the d-pad
and left stick move the highlight to the nearest element in that direction, and `A` activates it
alongside Enter, Space and the mouse. Read `Ui.IsFocused` to style the highlight and act on
`Ui.WasActivated`, and the same screen works with all three input devices:

```csharp
Ui.SetSelectable(_resumeButton);   // runtime-built UI opts in; authored widgets already have it
Ui.SetFocus(_resumeButton);        // land the highlight somewhere when the menu opens
...
if (Ui.WasActivated(_resumeButton)) { Resume(); }
```

The activating key or button is **consumed**, so it never also reaches the game underneath - `A`
confirms the menu item without the character also jumping on the frame the menu closes. That is
the whole reason to go through `Ui.WasActivated` rather than testing the button yourself.

If you prefer named actions over hard-coded buttons, `InputActions` binds a key and a pad button
to one name, so a rebinding screen has a single table to edit:

```csharp
InputActions.Register("jump", Key.Space, Key.None, GamepadButton.A);
...
if (InputActions.IsPressed("jump")) { }
```

Sticks are not actions — a direction is not a yes/no, so read `Gamepad.LeftStick` directly.

**If a controller seems dead**, ask the editor what it can see: the `engine_info` control method
lists every connected pad with its live stick, trigger and button state. An empty list means the
engine never saw it, which is a different problem from your code not reading it.

Rumble: `Gamepad.Rumble(low, high, seconds)` buzzes every connected XInput pad (Windows only) for
`seconds`, then stops itself; Stop and Pause stop it too. Not supported: gyro or touchpad input.

## Touching components from script

Anything the Inspector shows, a script can read and write, by the same names:

```csharp
// Typed, compiler-checked - preferred where a wrapper exists.
Self.Spin.Add();
Self.Spin.SetDegreesPerSecond(new Vector3(0, 90, 0));
float bobHeight = Self.Bob.Amplitude;
Self.Spin.Remove();

// Generic - works for every reflected component, including ones with no wrapper.
ComponentAccess sprite = Self.Component("Sprite Renderer");
sprite.SetVector4("tint", new Vector4(1, 0.3f, 0.2f, 1));
int layer = sprite.GetInt("sorting_layer");
```

Reads are properties, writes are `Set*` methods. That asymmetry is deliberate: the
wrappers are structs so they can be used as inspector fields, and C# rejects a property
assignment on a struct returned from a property (`CS1612`). A method always works.

Field names are the ones in the scene file and the `list_component_types` output.

## If the editor dies

The editor autosaves a recovery copy of the open scene whenever there are unsaved edits
— by default every 120 seconds, set by `app.autosaveSeconds` in `ProjectSettings.toml`
(0 turns it off).

It never writes your scene file. Copies land under `<project>/.aether/recovery/`, and a
real save deletes the copy it made redundant. So a recovery copy existing at all means
there is work the scene file does not have.

After a crash, reopen the project and ask:

```bash
./build/default/tools/control-client/RelWithDebInfo/aether-ctl.exe editor.recovery_list
```

Anything listed is newer than the scene it shadows. To promote one over the saved file:

```bash
./build/default/tools/control-client/RelWithDebInfo/aether-ctl.exe editor.recovery_restore '{"scene":"Level1"}'
```

Then reload the scene. Restoring is always explicit — nothing is promoted automatically,
and a recovery copy that does not parse is refused rather than written over a scene that
was merely out of date.

## Where to look next

| You want | Look at |
|---|---|
| Multiplayer - what it gives you, what it costs, and every failure mode | [Multiplayer](multiplayer.md) |
| Public scripting APIs and working examples | [Script APIs and examples](#script-apis-and-examples) |
| Audio playback, spatial sources and volume buses | [Audio](audio.md) and [C# Audio API](../managed/AetherCore/Audio.cs) |
| What a component's fields are called | `list_component_types` over the control endpoint, or the Inspector |
| Engine architecture | [Render-frame extraction](architecture/render-frame-extraction.md), [Asset database](asset-database.md) |
| Driving the editor from a script or agent | [tools/mcp/README.md](../tools/mcp/README.md) |
| Conventions, if you are changing the engine itself | [CONVENTIONS.md](CONVENTIONS.md) |

## Script APIs and examples

Start with the project's configured language and target. Use the existing CLI
`./scripts/Run-GameplayPipeline.ps1 -Project <root> -Operation inspect`, or call
`gameplay_job_start` with `project` and `operation: "inspect"`, then poll
`gameplay_job_status`. The [project inventory](architecture/gameplay-abi.md#read-only-project-discovery)
resolves script/build/SDK paths; it does not compile scripts or reflect their types.
Missing `[scripting].language` means C#. Keep edits in the project's resolved scripts
folder; the engine SDK is a dependency, not the place to write game behaviour.

### Public APIs

Read the actual declarations for signatures and lifecycle rules. Use the symbol graph
for precise navigation and graphify for subsystem relationships; if those tools are
unavailable or an index is stale, read these source files directly.

| Area | C# source of truth | Rust source of truth |
|---|---|---|
| Lifecycle and script fields | [EntityScript.cs](../managed/AetherCore/EntityScript.cs) | [lib.rs](../rust/aethercore/src/lib.rs): `Script`, `Properties`, `gameplay_script!` |
| Entities and transforms | [Entity.cs](../managed/AetherCore/Entity.cs), [World.cs](../managed/AetherCore/World.cs) | Same `lib.rs`: `Entity`, `Context::entity`, `position`, `set_position` |
| Input | [Input.cs](../managed/AetherCore/Input.cs), [InputActions.cs](../managed/AetherCore/InputActions.cs), [Gamepad.cs](../managed/AetherCore/Gamepad.cs) | Same `lib.rs`: `Context::key_down` |
| Physics | [Physics2D.cs](../managed/AetherCore/Physics2D.cs), [Physics.cs](../managed/AetherCore/Physics.cs), [CharacterController.cs](../managed/AetherCore/CharacterController.cs) | Same `lib.rs`: `Context::velocity_2d`, `set_velocity_2d` and `Script::event` |
| Components and rendering | [Component.cs](../managed/AetherCore/Component.cs), [ComponentRef.cs](../managed/AetherCore/ComponentRef.cs), [Behaviors.cs](../managed/AetherCore/Behaviors.cs), [SpriteRenderer.cs](../managed/AetherCore/SpriteRenderer.cs), [CustomPass.cs](../managed/AetherCore/CustomPass.cs) | Same `lib.rs`: `Context::set_sprite_color`, `register_custom_pass`, `unregister_custom_pass`, `submit_custom_pass`, `ComponentRef` fields and `gameplay_component_ref!`; check the available subset |
| Scenes, prefabs, UI, audio and saves | [Scene.cs](../managed/AetherCore/Scene.cs), [Assets.cs](../managed/AetherCore/Assets.cs), [Ui.cs](../managed/AetherCore/Ui.cs), [Audio.cs](../managed/AetherCore/Audio.cs), [SaveData.cs](../managed/AetherCore/SaveData.cs) | Same `lib.rs`: `Context::load_scene`, `instantiate_prefab`, `set_ui_text`, `play_audio`, `read_save`, `write_save` |
| Script registration | Project `scripts/AetherGame.csproj` and `EntityScript` subclasses | [provider.rs](../rust/aethercore/src/provider.rs): `export_gameplay!` |

The rest of the [C# SDK](../managed/AetherCore/) includes camera, animation, networking
and other topic files. The Rust authoring SDK is the safe `aethercore` crate; do not
start from raw `aethercore-abi` bindings or generated ABI files. The
[ABI tooling](../tools/gameplay-api/README.md) is for engine/interop maintenance.
C# and Rust do not promise identical surfaces. Check [Rust limitations](rust-gameplay.md),
[Web exports](web-export.md) and [C# browser limitations](csharp-web.md) for the target
before borrowing a desktop example.

### Working examples and agent loop

- **Small C# behaviour:** [EntityFollowerExample.cs](../projects/TestingProject/scripts/EntityFollowerExample.cs).
- **Paired gameplay examples:** [C# Reference.cs](../projects/AgentPipelineCSharp/scripts/Reference.cs)
  and [Rust reference.rs](../projects/AgentPipelineRust/scripts/src/reference.rs) exercise
  input, movement/physics, prefabs, UI, audio, saves and scene changes. The
  [reference comparison](gameplay-reference-comparison.md) explains their shared fixtures
  and verification. Each project has a `reference-test.json` launch configuration.
- **Rust fields and registration:** [fixture lib.rs](../projects/AgentPipelineRust/scripts/src/lib.rs)
  demonstrates property types and exports; it also contains deliberate host-test failure
  hooks, so use `reference.rs` first for ordinary gameplay. Fresh starter scripts are
  generated by [ProjectCommon.cpp](../src/app/project/ProjectCommon.cpp).

Inspect the project, read the relevant SDK declarations and example, edit the project,
then build and test through the [existing gameplay pipeline](architecture/gameplay-abi.md#agent-gameplay-pipeline).
Tests require explicit assertions: compiling or launching alone does not verify behaviour.
Use [agent iteration](agent-iteration.md) for build-once test cases and failure evidence.
For live editor work, use [MCP](../tools/mcp/README.md); the endpoint's `describe` manifest describes
editor operations, not the full public scripting SDK. Component reflection describes
editable component fields, and `list_script_types` lists loaded script types rather
than all available SDK calls.

## Browser games and autonomous distribution

[Agent iteration](agent-iteration.md) describes the native compiler caches, measured rebuild/run improvements and the repeatable CLI benchmark.

For Rust project creation, safe gameplay APIs and agent build/test operations, see [Rust gameplay](rust-gameplay.md). [Web exports](web-export.md) describes the supported 2D profile and publishing commands. [Distribution qualification](distribution-qualification.md) runs a fresh project through compiler repair, native tests, measured browser export and regression gates without a human editor session. C# stays the default for existing projects. [C# browser support](csharp-web.md) describes its qualified interpreter profile and SDK limitations.
