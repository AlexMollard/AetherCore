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

Start the Launcher. **Run it from the directory it was built into** — `shaders://` resolves
relative to the working directory, and starting it from anywhere else fails at pipeline
creation.

```powershell
cd build/default/src/app/RelWithDebInfo
./Launcher.exe
```

Choose **Blank 2D**, name it, pick a folder. The Launcher spawns an Editor for that project
and closes itself.

You get an orthographic camera, a `scripts/` folder with a starter `Player.cs`, and an empty
scene. The project folder is yours — scenes, assets, prefabs and scripts all live in it, and
`ProjectSettings.toml` at its root *is* the project.

## 2. Put something on screen

In the **Scene** panel, click **+** and add a Sprite. Select it and look at the **Inspector**:
a `Sprite Renderer` with no texture renders as a white quad, which is enough to see it move.

To use real art, drop a PNG into the project's `assets/` folder — the **File Explorer** panel
picks it up — then set it as the sprite's texture. For a sprite sheet, open the **Sprite
Slicer** and cut regions non-destructively; the slices keep stable identities if you re-cut later.

## 3. Make it move

`scripts/Player.cs` already exists. Open it (the **Project** panel's **Debug C#** button opens
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

Select your sprite, then **Add Component → Script → Player**. `Speed` appears in the Inspector
because it is a public field; edit it there and the value saves with the scene.

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

## Where to look next

| You want | Look at |
|---|---|
| The full scripting surface | `managed/AetherCore/` — one file per area (`Input`, `Physics2D`, `Ui`, `Scene`, `Time`, `Audio` is **not** among them yet) |
| What a component's fields are called | `list_component_types` over the control endpoint, or the Inspector |
| Engine architecture | [Render-frame extraction](architecture/render-frame-extraction.md), [Asset database](asset-database.md) |
| Driving the editor from a script or agent | [tools/mcp/README.md](../tools/mcp/README.md) |
| Conventions, if you are changing the engine itself | [CONVENTIONS.md](CONVENTIONS.md) |

## Known gaps

Worth knowing before you plan a game around them:

- **There is no audio.** No mixer, no `AudioSource`, no API. A game cannot make a sound today.
- **There is no gamepad support.** `Input` is keyboard and mouse only.

Both are tracked as the top of the roadmap rather than oversights.
