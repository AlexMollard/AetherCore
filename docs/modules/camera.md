# `camera/` - Cameras and lighting

The camera module owns scene cameras and all lighting state. Lighting is split into a global state (sun, ambient, sky) managed by `LightingManager` and per-light data (point, spot) accumulated by `Renderer`.

## Files

| File | Role |
|---|---|
| `Camera.hpp` / `Camera.cpp` | Single camera (view/proj matrix, near/far, FOV). |
| `CameraManager.hpp` / `CameraManager.cpp` | Manages a set of cameras, with a "main" camera concept. |
| `CameraSubsystem.hpp` / `CameraSubsystem.cpp` | Owns `CameraManager` + `LightingManager`, registers them. |
| `LightingManager.hpp` / `LightingManager.cpp` | Sun, ambient, sky, and per-frame light accumulation. |

## `Camera`

`src/engine/camera/Camera.hpp`. A single camera:

```cpp
class Camera {
public:
    void SetPerspective(float fovYRadians, float aspect, float nearZ, float farZ);
    void SetPosition(const glm::vec3& p);
    void SetRotation(const glm::quat& r);
    [[nodiscard]] glm::mat4 GetViewMatrix() const;
    [[nodiscard]] glm::mat4 GetProjectionMatrix() const;
    // ...
};
```

Cameras are value-typed; a `CameraHandle` is an index into `CameraManager`'s storage.

## `CameraManager`

`src/engine/camera/CameraManager.hpp`. Owns the set of cameras, with:

- `Create(CameraDesc)` → `CameraHandle` (or 0 = invalid).
- `GetMainCamera()` / `SetMainCamera(handle)`.
- `Update(Input&, dt)` - applies input to the main camera (only if the camera is flagged as `controllable`).

The main camera's view/proj are snapshotted into `RenderFramePacket` at `PrepareFrame` time.

## `CameraSubsystem`

`src/engine/camera/CameraSubsystem.hpp`. The subsystem façade. Owns `CameraManager` and `LightingManager`. Registers both on the service container. Init order is step 5 in `AetherCore::AetherCore`.

## `LightingManager`

`src/engine/camera/LightingManager.hpp`. Per-frame lighting state:

```cpp
class LightingManager {
public:
    void SetSun(const glm::vec4& directionIntensity, const glm::vec4& color);
    void SetAmbient(const glm::vec4& color);
    void SetSky(const glm::vec4& horizon, const glm::vec4& zenith, const glm::vec4& skyVoid);
    void AddPointLight(const Renderer::PointLight&);
    void AddSpotLight(const Renderer::SpotLight&);
    void ClearLights();
    void FlushTo(Renderer&);

    void LinkRenderer(Renderer&);
};
```

The `LinkRenderer` call from `AetherCore.cpp:121` connects it to the `Renderer` so `FlushTo` can push state into the per-frame state aggregator. `LightingManager` is the public API game code uses; `Renderer` is the internal model.

## Threading

- All camera and lighting state is engine-thread-only.
- Lighting state is snapshotted into `RenderFramePacket` at `PrepareFrame` time. The render thread never re-queries the manager.

## See also

- [`modules/rendering.md`](rendering.md) - `Renderer` and the packet snapshot.
- [`modules/scene.md`](scene.md) - how camera handles integrate with ECS.
