# `app/` - Application, layers, game systems

`src/app/` is the executable target. It owns the composition root: an `Application` runs the main loop, hosts a `LayerStack`, and registers game systems.

## Files

| File | Role |
|---|---|
| `Application.hpp` / `Application.cpp` | Main loop, coroutine executor, loading manager, layer stack. |
| `main.cpp` | Entry point. |
| `layers/` | App layers. |
| `systems/` | Game systems. |

## `Application`

`src/app/Application.hpp`. The main loop driver:

```cpp
class Application {
public:
    Application();
    int Run(AetherCore& engine);
    void PushLayer(std::unique_ptr<AppLayer> layer);
    void PopLayer(AppLayer& layer);
    // ...
};
```

Per-frame:

1. Pump events.
2. Run coroutine executor.
3. Tick all layers.
4. Tick `AetherCore`.
5. Submit the render packet.

`Application` owns the coroutine executor used for asset loading and other engine-thread coroutines.

## `AppLayer`

`src/app/layers/AppLayer.hpp`. The base interface for application layers:

```cpp
class AppLayer {
public:
    virtual ~AppLayer() = default;
    virtual void OnAttach(AetherCore& engine, ServiceContainer& services) = 0;
    virtual void OnUpdate(AetherCore& engine, ServiceContainer& services, float dt) = 0;
    virtual void OnUIRender(AetherCore& engine, ServiceContainer& services) = 0;  // optional
    virtual void OnDetach() = 0;

    [[nodiscard]] virtual std::string_view Name() const = 0;
};
```

Layers are pushed onto the `LayerStack` in a fixed order. `OnAttach` is called once at startup; `OnUpdate` every frame; `OnUIRender` after `OnUpdate` if the layer has UI.

## Built-in layers

| Layer | Role |
|---|---|
| `LoadingLayer` | Renders a full-screen loading overlay. |
| `DebugLayer` | Debug HUD, frame stats, Tracy overlay. |
| `SandboxLayer` | Generic sandbox for trying engine features. |
| `PhysicsLayer` | Drives the physics simulation. |
| `FishingLayer` | Game-specific fishing mini-game. |
| `InventoryLayer` | Game-specific inventory UI. |
| `UiSandboxLayer` | UI playground. |
| `ScriptedSceneLayer` | Loads and runs scripted scene data. |

## Game systems

`src/app/systems/`. Each system inherits from `aether::scene::System` (`src/engine/scene/System.hpp`) and is registered with the `World` for per-frame dispatch:

- **Day/night** - drives sun direction and color from elapsed time.
- **Fishing** - game logic for the fishing mini-game.
- **Physics** - drives `PhysicsSystem` at a fixed step.
- **Sandbox** - generic debug/iteration system.

## See also

- [`modules/engine.md`](engine.md) - `AetherCore` and `ServiceContainer`.
- [`modules/scene.md`](scene.md) - `System` interface.
- [`ARCHITECTURE.md §1`](../ARCHITECTURE.md) - subsystem init order.
