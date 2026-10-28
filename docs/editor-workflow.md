# Editor and gameplay workflow

[Back to the project overview](../README.md) · [First game tutorial](getting-started.md)

## Editor workflow

1. Start `Launcher` and create or open a project; it spawns a dedicated `Editor` process and closes itself.
2. Build a scene with the hierarchy, inspector, viewport, asset browser, and component tools.
3. Author gameplay with the project's selected C# or Rust SDK under its `scripts/` directory.
4. Enter Play to run it. For C# assembly replacement, **Stop first**, then use **F5 / Reload Scripts** in a development build. Requests during Play are rejected; arbitrary live script state is not migrated. Rust uses rebuild/relaunch.
5. Use **Debug C#** in the Project panel to open scripts in Visual Studio with the debugger attached, or pack and publish a standalone build.
6. **File > Save & Return to Project Launcher…** saves and hands control back to the hub when switching projects.

Each project is rooted by `ProjectSettings.toml` and owns its assets, scenes, prefabs, scripts, build intermediates, and publish settings. Engine resources mount through `engine://`, project content through `project://`.

## 2D workflow

Choose **Blank 2D** in the Launcher for an orthographic camera, 2D viewport grid and XY tools, and sprite rendering. It is the same project structure and editor shell as 3D, so scenes still share assets, C# gameplay, UI, packaging, and the render graph.

1. Import a texture or sprite sheet, then open the **Sprite Slicer** to create and refine non-destructive regions with grid, trim, and pivot tools.
2. Drag sprites into the 2D viewport or create **Sprite** and **Animated Sprite** entities from the hierarchy.
3. Create animation clips in the **Sprite Animation** timeline, edit frame durations and events, then preview playback in Edit or Play mode.
4. Save, script, and publish normally. Sprite regions keep stable identities across compatible re-slices, and published projects package their sprites, atlases, animations, and scenes.

Physics2D (Box2D) and chunked tilemaps have both shipped, alongside sprite authoring, animation, parallax, particles and 2D lighting.

## C# scripting

Managed gameplay is split across three assemblies with a deliberate boundary:

| Assembly | Role |
|---|---|
| `AetherCore` | Public gameplay SDK: entities, world, input, camera, physics, logging, and components |
| `AetherCore.Interop` | ABI and CoreCLR boot layer used by the native host; hidden from normal game code |
| `AetherGame` | Project gameplay scripts loaded into a collectible context for hot reload |

Scripts derive from `EntityScript`. Public fields appear in the editor inspector and serialize with the entity:

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

public sealed class Spinner : EntityScript
{
    public float DegreesPerSecond = 90.0f;

    public override void OnUpdate(float deltaTime)
    {
        Vector3 euler = Self.EulerDegrees;
        euler.Y = (euler.Y + DegreesPerSecond * deltaTime) % 360.0f;
        Self.EulerDegrees = euler;
    }
}
```

The CMake build runs `dotnet build` when the .NET SDK is present and deploys the managed assemblies next to the native executable. NuGet dependencies declared by the game project resolve into the script load context.

For exact declarations, SDK boundaries and working C#/Rust examples, start with
[Script APIs and examples](getting-started.md#script-apis-and-examples).
[Rust gameplay](rust-gameplay.md) covers the safe Rust SDK, project creation,
rebuild/relaunch and publishing; Rust does not use C#'s F5 hot-reload path.

## Rust gameplay

Select Rust when creating a project. The safe SDK supplies `Script`, `Context`, registration macros and access to the shared gameplay host; desktop builds load the native module, while browser publishing links the selected game into the WebAssembly player. C# remains the default for projects without language metadata.

Rust currently uses **rebuild/relaunch**, not the C# assembly-replacement path. Start with [Rust gameplay](rust-gameplay.md) and [paired SDK examples](getting-started.md#script-apis-and-examples).

## Editor automation

For live editor control, screenshots and offline gameplay jobs, use the [MCP setup guide](mcp-setup.md) and [engine MCP reference](../tools/mcp/README.md).
