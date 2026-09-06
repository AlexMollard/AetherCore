# Asset credits

All assets below are CC0 / Public Domain. Attribution is not legally required by
any of these licences but is recorded here per project convention.

## Kenney — Factory Kit (v3.0)
- Source: https://kenney.nl/assets/factory-kit
- Author: Kenney (www.kenney.nl)
- Licence: CC0 1.0 Universal (https://creativecommons.org/publicdomain/zero/1.0/)
- Files used (copied from `Models/GLB format/`):
  - `box-small.glb` -> `Props/CrateSmall.glb`
  - `box-large.glb` -> `Props/CrateLarge.glb`
  - `box-wide.glb` -> `Props/CrateLong.glb`
  - `cone.glb` -> `Props/Cone.glb`
  - `structure-wall.glb` -> `Environment/StructureWall.glb`
  - `floor-large.glb` -> `Environment/FloorLarge.glb`
  - `button-floor-round-small.glb` -> `Devices/Button.glb`
  - `lever-single.glb` -> `Devices/Lever.glb`
  - `door.glb` -> `Devices/Door.glb`
  - `pipe-large-valve.glb` -> `Devices/Emitter.glb`
  - `screen-small.glb` -> `Devices/Gate.glb`
  - `Textures/colormap.png` -> `Props/Textures/colormap.png`, `Environment/Textures/colormap.png`,
    and `Devices/Textures/colormap.png` (shared colour atlas referenced by every
    Factory Kit `.glb` above by relative path)
  - `cog-a.glb` -> `Props/CogLarge.glb`
  - `cog-b.glb` -> `Props/CogMedium.glb`
  - `piston-round.glb` -> `Props/Piston.glb`
  - `machine.glb` -> `Props/MachineBlock.glb`
  - `hopper-round.glb` -> `Props/Hopper.glb`
  - `pipe-large.glb` -> `Props/PipeSegment.glb`
  - `arrow-basic.glb` -> `Props/ArrowSign.glb`
  - `warning-traffic.glb` -> `Props/WarningSign.glb`
  - (these 8 also reference `Props/Textures/colormap.png` above, same shared atlas)
  - `machine-fortified.glb` -> `Props/MachineFortified.glb`

## Kenney — Furniture Kit (v2.0)
- Source: https://kenney.nl/assets/furniture-kit
- Author: Kenney (www.kenney.nl)
- Licence: CC0 1.0 Universal (https://creativecommons.org/publicdomain/zero/1.0/)
- Files used (copied from `Models/GLTF format/`), all self-contained (flat
  vertex-colour materials, no external textures):
  - `cardboardBoxClosed.glb` -> `Props/CardboardBox.glb`
  - `trashcan.glb` -> `Props/TrashCan.glb`
  - `chair.glb` -> `Props/Chair.glb`
  - `table.glb` -> `Props/Table.glb`
  - `bench.glb` -> `Props/Bench.glb`
  - `pottedPlant.glb` -> `Environment/PottedPlant.glb`
  - `bookcaseOpen.glb` -> `Environment/Bookcase.glb`
  - `lampWall.glb` -> `Devices/Lamp.glb`

## Quaternius — "Barrel" (via Poly Pizza)
- Source: https://poly.pizza/m/MraIiFnpAY
- Author: Quaternius (https://quaternius.com)
- Licence: CC0 / Public Domain
- File used: `Props/Barrel.glb`

## Quaternius — "Man" (via Poly Pizza) — evaluated, NOT wired into the ragdoll catalogue
- Source: https://poly.pizza/m/HMnuH5geEG
- Author: Quaternius (https://quaternius.com)
- Licence: CC0 / Public Domain
- File: `QuaterniusMan/QuaterniusMan.glb`
- Imported and rendered correctly, but its skeleton uses Quaternius's own
  "HumanArmature" bone names (`Torso`, `Abdomen`, `UpperArm.L`, `LowerArm.L`,
  `Foot.L`, ...), not the `mixamorig_*` convention the existing `Human.gltf`
  uses and the ragdoll bone-mapper expects. It will not drive ragdoll bodies
  without engine-side bone-name-alias work. See the report for detail.

## `IconScratch/` — self-authored, NOT third-party assets
- Six tiny scratch `.glb` files (`SmallCrate.glb`, `MediumCrate.glb`, `HeavyCrate.glb`,
  `Marble.glb`, `Ball.glb`, `HeavyBall.glb`): a plain unit cube or unit UV-sphere mesh
  each, generated programmatically and tinted to match the corresponding `PropDef`
  entry's existing colour in `PropSpawner.cs`. They exist solely as inputs to the
  `ModelPreviewService` icon baker (see `PropCatalogExtension.md`) so the primitive
  catalogue entries (which have no source model at all - they're built-in
  cube/sphere primitives at runtime) get a spawn-menu icon through the same bake
  pipeline as the licensed models, for consistent framing/lighting/background.
- No CC0/third-party sourcing applies - authored by this session, not downloaded.
  Not intended as shippable/reusable art beyond producing the six baked icon PNGs;
  kept in their own folder, separate from the licensed models above, precisely so
  they are never mistaken for one.

## Kenney — Blaster Kit (v2.1) — first-person viewmodels, NOT physics props
- Source: https://kenney.nl/assets/blaster-kit
- Author: Kenney (www.kenney.nl)
- Licence: CC0 1.0 Universal (https://creativecommons.org/publicdomain/zero/1.0/)
- Files used (copied from `Models/GLB format/`):
  - `blaster-l.glb` -> `Viewmodels/PhysicsGunViewmodel.glb` (widest/chunkiest of the 18
    variants - picked for a gravity-gun-style silhouette)
  - `blaster-e.glb` -> `Viewmodels/ToolGunViewmodel.glb` (longest/slimmest variant -
    picked for a laser/beam-tool silhouette)
  - `Textures/colormap.png` -> `Viewmodels/Textures/colormap.png` (shared colour atlas
    referenced by both `.glb` files above by relative path)
- **These are first-person viewmodels, not spawnable/physics props.** No collider is
  fitted or needed - see `PropCatalogExtension.md`'s Viewmodels section. Wiring them
  onto the camera in `PhysicsGun.cs` / a tool-gun script is out of my file boundary;
  flagged to Main for routing.

## Self-authored construction-stock props — NOT third-party, unique per the user's ask
- Six `.glb` files in `Props/`: `SteelPlate.glb`, `Beam.glb`, `Wheel.glb`,
  `HingePlate.glb`, `BallJoint.glb`, `ThrusterBody.glb`. Plain parametric geometry
  (box/cylinder/sphere/frustum) generated programmatically this session using the same
  GLB-packing pipeline built earlier for `IconScratch/`'s six primitive icons, each
  with its own baseColorFactor/metallic/roughness and a real `TEXCOORD_0` (the missing
  attribute that broke `IconScratch/`'s colours - present here from the start).
- Not downloaded from any kit; these are the "genuinely unique" half of the 30-asset
  requirement. `HingePlate` is visually a plain flat plate, not a modelled hinge
  knuckle - the actual hinge is a physics constraint applied by the wiring/tool system
  at spawn time, not a mesh feature, so a fabricated hinge-barrel mesh that wouldn't
  actually rotate would be dishonest geometry; kept simple and said so here instead.
- No CC0 sourcing question applies - authored by this session. Not intended as
  shippable/reusable art outside this project.
