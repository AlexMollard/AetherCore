# Interactable device models — sourcing report

The user's ask: "everything must have a visual" - today Button, Lever, Lamp, Door,
Emitter and Gate are all untextured coloured primitive cubes. This finds a real CC0
model for each, sized and collider-fitted against `Sandbox.scene.toml`'s existing test
entities (all placed at `y=1.6`, all `half_extents ~ (0.25,0.25,0.25)` i.e. ~0.5m cubes
today - see `Test Button`/`Test Gate`/`Test Lamp` in the scene file), so a straight
model swap doesn't wildly change scale.

**Status: sourced, licence-recorded, colliders derived analytically. NOT YET verified
live** - the Editor was held by `project-audit` for the mouse-input bugs throughout this
work. Every number below needs the same live check the prop catalogue got: place each
model with a size reference, confirm by shape (not by filename), confirm the collider
fit, before treating this as final. Do not skip that step just because the geometry
math looks right on paper - that exact overconfidence produced two wrong conclusions
elsewhere in this project today.

All models are re-uses of packs already fully licence-checked for this project
(Kenney Factory Kit v3.0, Kenney Furniture Kit v2.0 - both CC0 1.0 Universal,
`kenney.nl`) - see `CREDITS.md` for the per-file entries. No new licence research was
needed; these are additional files pulled from packs already downloaded and verified
this session.

## Per-device sourcing

| Device | Model | Source file | Native size (m, W×H×D) | Fit quality |
|---|---|---|---|---|
| Button | `Devices/Button.glb` | Factory Kit `button-floor-round-small.glb` | 0.300 × 0.100 × 0.300 | **Adapted, not purpose-built.** This is Kenney's floor-mounted elevator-style push-plate, reused as a general push-button since Factory Kit has no free-standing wall/panel button. Reads clearly as "a button" (round, low-profile, pressable) at any mount height; the only give is that its underside implies floor-flush mounting rather than a panel face. Honest, not a bad-fit - flagging the adaptation rather than pretending it's purpose-made. |
| Lever | `Devices/Lever.glb` | Factory Kit `lever-single.glb` | 0.500 × 0.475 × 0.340 | **Good fit.** A genuine lever arm on a base - this is exactly the "reads as having two positions" shape Main asked for; the arm's resting angle can visually communicate on/off even before `Lever.cs`'s emissive tint kicks in. |
| Lamp | `Devices/Lamp.glb` | Furniture Kit `lampWall.glb` | 0.227 × 0.093 × 0.150 | **Good fit.** A compact wall-sconce light fixture - exactly the "mesh is the missing half" Main described, since `Lamp.cs` already drives a real Point Light's intensity. Flat `baseColorFactor` material (no texture), self-contained. |
| Door | `Devices/Door.glb` | Factory Kit `door.glb` | 0.800 × 1.600 × 0.300 | **Good fit, with a framing caveat.** Reads unambiguously as a door. `Door.cs`'s default `OpenOffset` is `(0, 2.5, 0)` - vertical - so this reads as a roll-up/vertical-lift door once it slides, not a hinged swing door, which is a legitimate door archetype (warehouse doors, elevator doors) but worth being explicit about so nobody expects it to swing. |
| Emitter | `Devices/Emitter.glb` | Factory Kit `pipe-large-valve.glb` | 1.000 × 1.000 × 1.016 | **Reasonable fit, recommend scaling down.** A pipe valve/spigot - reads as "a nozzle something comes out of," matching "spawner/nozzle" well. Native size is roughly 2x the other devices' ~0.5m scale; recommend placing at `scale ~0.5` to match (see table below), not a fault of the model itself. |
| Gate | `Devices/Gate.glb` | Factory Kit `screen-small.glb` | 0.600 × 0.991 × 0.499 | **Deliberate choice, per Main's own suggestion.** Not a "realistic" logic gate (there isn't a real-world object for that) - a small industrial screen/readout panel, which reads as "this is a device that displays/processes information" without pretending to be something concrete it isn't. Native size is taller than the other devices; recommend `scale ~0.5-0.6` to bring it to roughly the same footprint. |

## Proposed collider fits (analytical - NOT live-verified, see status above)

Center-offset convention matches `PropCatalogExtension.md`'s derivation: box colliders
get `center = ((minX+maxX)/2, half_extents.y, (minZ+maxZ)/2)` so the collider's bottom
face sits at the entity's own origin instead of straddling it, matching every one of
these models' base-pivoted convention (confirmed `min.y = 0` on all six, same as every
Kenney model used elsewhere in this project).

| Device | Collider shape | Params | Notes |
|---|---|---|---|
| Button | Cylinder | radius 0.15, half_height 0.05, center (0, 0.05, 0) | Matches its round, flat-disc silhouette closely - the one device here where Cylinder is unambiguously the right primitive, not an approximation. Confirm the addable `Cylinder Collider` catalog entry (found working earlier this session) still applies before relying on it. |
| Lever | Box | half_extents (0.354, 0.238, 0.17), center (0, 0.238, 0) | **Sized to the swept arc, not the resting pose - see decision below.** |
| Lamp | Box | half_extents (0.1135, 0.0465, 0.075), center (0, 0.0465, 0.075) | Small, low-risk fit - a wall sconce's collider mattering little since nothing needs to collide with a lamp specifically. |
| Door | Box | half_extents (0.4, 0.8, 0.15), center (0, 0.8, 0) | This is the collider Main flagged as needing to "actually shove what leans on it" - a door is genuinely box-shaped, so a tight box fit here should be accurate, not an approximation like the props' chair/table cases. |
| Emitter | Box | At native scale: half_extents (0.5, 0.5, 0.508), center (0, 0.5, -0.008). **At recommended scale 0.5x: half_extents (0.25, 0.25, 0.254), center (0, 0.25, -0.004).** | A valve's actual silhouette (wheel + body) isn't box-shaped, but nothing needs to collide precisely with an Emitter the way Button/Lever/Door do - a loose box is an acceptable approximation here, not a priority to refine. |
| Gate | Box | At native scale: half_extents (0.3, 0.4955, 0.2495), center (0, 0.4955, 0.0785). **At recommended scale 0.5x: half_extents (0.15, 0.24775, 0.12475), center (0, 0.24775, 0.039)** | Pure logic node, no physical interaction expected beyond the wiring raycast - a box is the right level of effort here. |

### Lever collider decision: swept arc, not resting pose

`lever-single.glb` has three baked animation clips (`toggle-on`, `toggle-off`,
`toggle`) that rotate the handle node ~±50° around its hinge, with the `toggle` clip's
overshoot briefly reaching ~60° before settling - confirmed by reading the glTF
animation samplers directly, not assumed. **`Lever.cs` does not currently play any of
them** - `Interact()` only flips `Self.Material.SetEmissive`, so today the handle
always renders at its authored bind pose (upright, centred) regardless of on/off state.

That makes the bind-pose-only box technically correct for CURRENT behaviour, but I
chose the swept-arc box anyway: this model was clearly authored to be animated (three
named clips ready to go, not incidental), wiring `Interact()` to actually play
`toggle-on`/`toggle-off` is an obvious and likely next step for whoever owns `Lever.cs`,
and a collider that already covers the swing costs nothing today (the box is only
~0.35m vs ~0.25m half-width - not a footprint change anyone will notice) while avoiding
a second bug-hunt identical to this session's Bench/Chair saga once someone wires up
the animation and the handle starts swinging outside a resting-pose-only collider.
Computed by unioning the base mesh's bounds with the handle mesh's bounds rotated by
every keyframe quaternion across all three clips (61 keyframes total) - not eyeballed.
Kept the base's collider on the SAME box as the swept handle (one collider, one
entity) rather than Main's alternative of "static base only, arm visual-only", since
the swept box is barely larger than the base alone (0.354m vs 0.25m half-width) and
keeps the whole device raycastable as one shape rather than needing two colliders on
one interactable.

### Door mid-slide: no swept-arc issue, checked and ruled out

`Door.cs` moves the door via `Physics.SetMotionType(Kinematic)` + `Self.Position`
writes (a straight translation, confirmed by reading the script - no rotation, no
skeletal animation). A Kinematic body's collider is rigidly attached to its entity
transform and translates WITH it every step (`PushKinematicTargets`, per the script's
own file comment) - the box collider fitted to the door's own static shape is correct
at every point along the slide, not just at the two endpoints. Nothing to resize here;
flagging that this was checked rather than assumed, since Main asked for it
specifically.

## Icons

Not yet generated - needs the Editor, via the individual-placement path (not the File
Explorer atlas, per the standing instruction and this session's own recorded process
lesson about judging one prop in isolation). Plan: place each device next to a
known-size reference (one of the already-verified prop icons' source models, e.g.
`CrateSmall.glb` at its confirmed 0.595m width, works well as a human-scale-adjacent
ruler), screenshot, verify shape, THEN crop/key/bake the icon - same order as the
Bench/Chair correction, front-loaded this time instead of learned the hard way again.

## What was rejected / not attempted

Did not go hunting outside the two packs already downloaded and licence-verified this
session (Kenney Factory Kit, Kenney Furniture Kit) given time constraints and that both
already had a plausible match for every one of the six devices - a new pack means new
licence research for marginal gain here. If any of the six fits above are judged too
loose once seen live (Button's floor-plate adaptation is the most likely candidate),
the next-best option is a fresh search specifically for "big red button" or
"industrial push button" CC0 models, not yet done.
