# Human Demo

A fully hierarchy-driven sample scene — no runtime spawner scripts. Everything is
placed directly in `scenes/HumanDemo.scene.toml`:

- **Human** — a skinned Mixamo character (`assets/models/Human/Human.gltf`). The model
  carries two animation clips: clip 0 is the embedded idle, clip 1 is a walk cycle
  (retargeted from a Mixamo "Walking" clip, merged into the model's glTF so it bakes
  into the same animset). The scene plays the **walk** via its `skinned` component.
- **Ground** — a static box the character walks on.
- **Lighting** — a key / fill / rim point-light rig grouped under an empty `Lighting` transform.
- **Camera** — an orbit camera framing the character.

Assets live under `assets/`, scenes under `scenes/`, scripts under `scripts/`.

> Note: baked model artifacts (`.mesh`, `.skel`, `.animset`, `.anim`, `.material`) are
> git-ignored — the editor regenerates them from `Human.gltf` on first import.
