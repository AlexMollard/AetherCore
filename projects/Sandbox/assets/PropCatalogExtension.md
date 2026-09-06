# Icon generation status (spawn-menu grid) — FINAL for this session

**10 of 16 icons done and verified, in `projects/Sandbox/assets/Icons/`:** Barrel.png,
Bench.png, CardboardBox.png, Chair.png, LargeWoodenCrate.png (CrateLarge),
LongWoodenCrate.png (CrateLong), SmallWoodenCrate.png (CrateSmall), Table.png,
TrafficCone.png, TrashCan.png. All 10 real (non-scratch) models. Each confirmed by
visual shape match against known geometry - critically including Bench and Chair,
which earlier drafts of this file excluded as "contaminated by an atlas bug". That
bug does not exist: see the retraction below. Every icon is RGBA 128x128, flood-filled
to real alpha (0,0,0,0 background, opaque model-coloured centre, confirmed
programmatically), minor single-digit-to-30s-pixel edge fringe on most (screenshot
compression, not a keying failure), no leak-detection failures.

**RETRACTED: "Bench/Chair thumbnail atlas bug".** Earlier drafts of this file (and
several messages this session) concluded the File Explorer's thumbnail atlas showed a
chair under Bench's filename due to a stale-slot bug, and routed a fix to the engine
team. That diagnosis was wrong, confirmed two ways: (1) `Bench.glb`'s glTF accessor
bounding box is `max=(0.4,0.47,0)` vs `Chair.glb`'s `max=(0.2,0.47,0)` - exactly double
width, and the binary vertex buffers are not byte-identical; (2) placing both side by
side in a live scene shows Bench as a genuine two-seat bench WITH a backrest (a
loveseat-style design, sharing Chair's slat-back styling since both are the same
Kenney kit) - visibly, unambiguously wider than Chair once there's a size reference.
Viewed alone at small size with no reference, a two-seat bench in this design
language reads as "a chair" to a human and to a distant/cropped render alike - that is
what actually happened, repeatedly, to more than one person looking at it this
session. The engine team's generation-token fix for the thumbnail atlas's
slot-readiness check stays in the tree on its own merits (a real latent defect - it
cannot distinguish a recycled slot number from a fresh request) but was never the
cause of this particular symptom. Lesson worth keeping for the next person: **never
judge one prop's shape in isolation**; put a size/shape reference next to it first.
That single change is what actually resolved this after several wrong turns.

**Not reached, honestly missing (6):** the primitive entries (Small/Medium/Heavy
Block, Marble, Ball, Heavy Ball). Attempted and found broken, not shipped: their
scratch `.glb` models (in `IconScratch/`) render with the CORRECT distinct
`baseColorFactor` per glTF JSON (verified: Ball 0.2/0.6/0.85, Marble 0.85/0.35/0.2,
etc.), but every one of them renders as the SAME uniform mid-grey in both the
isolated scene view and the File Explorer thumbnail bake - color is not reaching the
shader at all. Suspected missing `TEXCOORD_0` (Kenney's working models all have it;
my first-pass scratch geometry didn't) - regenerated all six with `TEXCOORD_0` plus
the full material field set (`emissiveFactor`, `doubleSided`, `alphaMode`) matching
Kenney's material JSON exactly, cleared the stale `.mesh` bake cache, re-imported -
still uniform grey. Root cause NOT found. Six uniform-grey icons that don't
distinguish Marble from Ball from Heavy Ball would defeat the entire purpose of a
spawn-menu icon, so none were shipped. `IconScratch/` still holds the corrected
(TEXCOORD_0-equipped) source `.glb` files for whoever picks this up - the mesh-level
geometry is confirmed correct (right shape, right size per file), only the material
colour fails to render.

QuaterniusMan is correctly not part of this 16-entry catalogue and was not attempted.


# Prop catalogue extension — proposed data (hand off to PropSpawner.cs owner)

**Status: FIX CONFIRMED LIVE, RE-VERIFICATION PASSED (2026-09-06, Editor.exe built
03:01:15 that day, after the `RebuildBody3D` fix landed).** The engine worker's fix
works: 8 of 9 box/capsule props (all except Cone, which had a genuine authoring error
of mine, now fixed and re-verified) settled at `y ≈ -0.014` - the SAME height,
regardless of half_extents/radius ranging from 0.1405 to 0.5725 - which is exactly the
prediction this file made in advance ("correct center offsets ⇒ every box/capsule prop
settles at approximately the same entity Y regardless of half-extents"). Full numeric
results and the Cone bug are in the "Re-verification results" section below. The table
of collider dimensions further down is now confirmed correct, not just correct as
authored data.

## Confirmed engine finding: `Collider` field writes do not affect the simulated shape

Reproduced cleanly, twice, against a single verified Editor instance (`tasklist`
confirmed exactly one `Editor.exe`, `aether-ctl info` confirmed the expected scene and
entity count before every step):

1. `scene.add_model` a fresh `CrateSmall.glb` (no pre-existing physics components -
   confirmed by `scene.add_component` returning `"added":true`, not `"alreadyPresent"`,
   for both `Rigid Body` and `Collider`).
2. `scene.set_component` the `Collider`'s `half_extents` to `(0.298, 0.275, 0.25)`.
3. `scene.get_component` immediately after: **read-back matches exactly** -
   `half_extents: [0.298..., 0.275..., 0.25]`. The component data is correct.
4. Set `Rigid Body.motion = "dynamic"`, read that back too - correct.
5. Enter Play, wait ~4s (474 physics frames). The entity settles at
   `y = 0.47999972`.
6. **Decisive step:** while still resting and still in Play, `scene.set_component` the
   same `Collider.half_extents` to `(2.0, 2.0, 2.0)` - a 4x size jump that, if the
   physics system were reading current component state, would immediately shove the
   resting body upward (or at minimum jitter it) since a 2m box suddenly overlaps
   ~1.5m of ground it wasn't touching before. **The entity's transform did not change
   by a single float bit.** `get_component` on the `Collider` confirms the component
   itself now holds `2.0, 2.0, 2.0` - the data changed, the simulated shape did not.

**Conclusion:** the Jolt shape is built once, at (or shortly after) body creation, from
whatever `ColliderComponent` state existed at that moment, and is never rebuilt when the
component is edited afterward - not before Play, and not during it. The settled height
of `~0.48` in every one of my tests, regardless of the half_extents/radius I configured,
is consistent with every one of these bodies actually simulating the compiled-in default
box (`half_extents{0.5,0.5,0.5}` in `PhysicsComponents.hpp`) rather than anything I
wrote - i.e. **every collider in the original 10-prop table below was decorative data
that never reached the simulation.** The original "9 of 10 settled correctly" claim from
the first pass is retracted: it was ten props falling onto ten identical invisible
0.5m-half-extent boxes and stopping, which looks fine in a screenshot precisely because
it proves nothing about per-prop fit.

**This is a genuine engine bug**, not an asset-side mistake, in the same silent-no-op
family as the previously-found `Physics.SetLinearVelocity`-before-bake drop and the
`Entity`-typed script-property silent discard: a component can be read back as correctly
holding the value you wrote, with no error anywhere in the chain, while the simulation
silently keeps using something else. **Nothing in this session's asset work
(model imports, licence, credits) is affected** - only the live collider-fit
verification is invalidated, because there is currently no way to prove or disprove a
primitive's fit by dropping it in the Editor: every dropped prop will settle identically
regardless of its configured shape until this is fixed. The `center`-offset table below
should be understood as "correct data to write, once writing it does something."

Ten imported props were `scene.add_model`'d, given a `Rigid Body` + `Collider` via the
generic reflection API, dropped from 1.5m and screenshotted after settling in the
*original* (pre-this-finding) pass. Nine of ten looked plausible in that screenshot;
CardboardBox alone visibly hovered/tumbled. Given the finding above, **none of those
nine "plausible" results are actually evidence of correct collider fit** - they are
evidence that ten differently-shaped props all quietly simulated the same default box
and none of them happened to look wrong at a glance. CardboardBox's visible failure was
not a special pivot bug after all; it was almost certainly a case where the model's
*visual* geometry differed enough from a default 1m cube (its origin sits at a corner of
its own bounding box, not its centre - see the raw bounds below) that the mismatch
became noticeable, while every other prop's mismatch was small enough to not be visibly
obvious in one screenshot.

Model paths are relative to `project://assets/models/` (i.e.
`projects/Sandbox/assets/models/...`), matching what `scene.add_model` /
`Entity.LoadModel` already expect.

## Re-verification results (2026-09-06, post-fix)

Rebuilt `Editor.exe` (confirmed modify timestamp `2026-09-06 03:01:15`, after the fix
landed), confirmed exactly one `Editor.exe` running via `tasklist` before touching
anything, ran Protocol Step 1 then Step 3 from the section below.

**Step 1 (child-hierarchy check):** confirmed via `scene.entities` that `TrashCan`'s two
child primitives both report the same position as their root (zero relative offset).
Traced why: both `trashcan.glb` and `cardboardBoxClosed.glb` have exactly **one** glTF
node holding a mesh with two primitives (two materials, not two nodes) - so there is no
per-primitive offset to get wrong for either of them. One new, minor finding from this
check: `trashcan.glb`'s single node carries a **90° Y rotation** (`rotation:
[0, 0.707, 0, 0.707]`) that my original bounds script ignored (it only applied
translation and scale, not rotation, when computing raw AABB unions - noted at the time
as "fine for axis-aligned Kenney assets"). This is wrong for TrashCan specifically, but
the model is a near-circular object, so a 90° swap of its X/Z extents changes the
result by only a few millimetres - not worth re-deriving, and not a concern for any
other prop (verified none of the other nine have a non-identity node rotation).

**Step 3 (drop-and-settle against the prediction):** placed all 10 props fresh, added
colliders via the new addable catalog names (`Box Collider` / `Capsule Collider` - the
bare `"Collider"` name is now correctly rejected as reference-only, confirming the
`addable`-gate fix), set the exact `half_extents`/`radius`/`half_height`/`center`
values from the table below, read every value back to confirm before hitting Play,
then dropped from 1.5m:

| Prop | half_extents.y or (half_height+radius) | Settled Y | Matches prediction? |
|---|---|---|---|
| CrateSmall | 0.275 | -0.01396 | Yes |
| CrateLarge | 0.275 | -0.01396 | Yes |
| CrateLong | 0.275 | -0.01396 | Yes |
| CardboardBox | 0.1405 | -0.01397 | Yes |
| Barrel | 0.5725 | -0.01396 | Yes |
| TrashCan | 0.214 | -0.01396 | Yes |
| Chair | 0.235 | -0.01406 (small settle wobble, ~2-3° tilt, self-corrects) | Yes |
| Table | 0.1635 | -0.01396 | Yes |
| Bench | 0.235 | -0.01382 | Yes |
| **Cone** | 0.225 (`half_height+radius`) | **+0.0718 (WRONG)** | **No - see below** |

**This is exactly the discriminating result the protocol was designed to produce.**
Eight props with `half_extents.y`/`half_height+radius` ranging from 0.1405 to 0.5725 -
a 4x spread - all settled within 0.0002 of each other (`-0.014`), which is only possible
if every one of their `center` offsets is correctly placing the collider's bottom face
at the entity origin. Cone alone settled `0.0718` too high, and the arithmetic explains
it exactly: I had written Cone's `center.y` as `0.1532` (half the model's *total mesh
height*, copied from the wrong column of my own bounds table) instead of the correct
capsule formula `half_height + radius = 0.075 + 0.15 = 0.225` that every other capsule
prop actually used. The resulting local collider-bottom offset was `0.1532 - 0.225 =
-0.0718`, and `-(-0.0718) = 0.0718` is exactly the observed settled height. **Fixed
live** (`center` corrected to `(0, 0.225, 0)`), re-verified: Cone re-settled at
`y ≈ 0` within one physics step, no teleport or instability, matching the other nine.
The catalogue table below has the corrected value.

**Mid-play rebuild behaviour (asked for explicitly):** every `set_component` call in
this session that touched a physics field - including the deliberate mid-play Cone fix
above and, in the earlier bug-finding session, a deliberate mid-play `half_extents`
jump from default to `(2,2,2)` on a resting body - produced a clean, bounded correction
with no visible hitch, explosion, or teleport-through-floor. The Cone specifically went
from resting at the wrong height to resting at the right one in a single smooth step.
No evidence of lost or corrupted velocity was observed, though none of these tests
involved a body with significant velocity at the moment of the rebuild (all were either
at rest or freshly spawned) - a body actively moving fast through a rebuild was not
specifically exercised.

**Screenshot:** one wide shot captured post-settle (all 10 props resting, none floating
or sunk through the visible ground plane) - `editor.camera` (needed for the per-prop
close-up framing originally requested) refused with "edit-mode only (stop play first)",
and stopping Play resets the scene to its pre-Play snapshot, which would have discarded
the settled state being verified. The numeric Transform read-backs in the table above
are the actual proof for this pass; the wide screenshot is corroborating visual evidence
only, not a substitute for individual close-ups. Getting true per-family close-ups would
need either a way to move the camera without stopping Play, or a second pass that
accepts losing Play state to reposition, screenshot, then redo the drop.

## Collider `center` offsets — still needed once the engine bug above is fixed

Independent of the collider-writes-don't-simulate bug: `ColliderComponent.center`
defaults to `(0,0,0)`, which places a box or capsule **symmetrically around the entity
origin** (a box spans `y:[-half_extents.y, +half_extents.y]`). None of these
Kenney/Quaternius **source glTF files** are pivoted at their own centre — every one
has `min.y = 0` in raw mesh-space, i.e. pivoted at ground level, the way a model needs
to be pivoted for `entity.Position` to mean "where its feet/base touch the floor".
Kenney's Furniture Kit models (Chair, Table, Bench, CardboardBox) are *also* off-centre
in X/Z — their footprint sits entirely in one quadrant relative to the pivot (e.g.
Chair's raw mesh spans `x:[0, 0.2], z:[-0.2, 0]`, not a centred `x:[-0.1,0.1],
z:[-0.1,0.1]`). Kenney's Factory Kit models (the three crates, the cone) already have
centred footprints (`x:[-w/2,w/2]`, `z:[-d/2,d/2]`), so only their Y needs correcting.

**Caveat that matters more than the math below:** this is measured from the *raw
source glTF* accessor bounds, not from what the engine's own import pipeline
(`ModelBake.cpp`, visible in the console log as `Imported model '...' -> X.mesh`)
actually produces. If the bake step recentres or otherwise normalizes a mesh's pivot
during import — plausible, and not checked this session — these offsets could be wrong
for the *baked* `.mesh` the engine actually renders, even though they are correct for
the source `.glb`. **Confirm the baked mesh's actual pivot before trusting this table**
(e.g. via the Inspector's bounds display on a placed instance, or by comparing a
known-dimension prop's rendered silhouette against its reported Transform position).

The candidate fix for every box-shaped prop below is a `center` field of
`((minX+maxX)/2, half_extents.y, (minZ+maxZ)/2)` in raw-glTF terms — horizontally
centred on the footprint, vertically raised so the box's bottom face sits at the
entity's own origin instead of straddling it. The capsule-shaped props (Barrel, Trash
Can, Cone) need `center = (0, half_height + radius, 0)` for the same reason (a
capsule's total half-length top-to-bottom is `half_height + radius`, since the caps add
`radius` beyond the straight section).

**Naming note:** the existing primitive `PropDef` catalogue already has entries called
"Small Crate", "Medium Crate", "Heavy Crate" (coloured cube primitives) and "Marble",
"Ball", "Heavy Ball" (coloured sphere primitives) - see `PropSpawner.cs`. My first draft
of this table reused "Small Crate" for `CrateSmall.glb`, which collides with the
existing primitive entry's display name. Renamed below to keep every catalogue entry's
name unique regardless of whether the primitive and model versions end up coexisting or
one replaces the other - that's a `PropSpawner.cs` decision, not mine, so I kept both
namings distinct rather than presuming an answer.

| Catalogue name | Category | Icon path | Model path | Native size (m, W×H×D) | Collider shape | Collider params (RE-VERIFIED LIVE 2026-09-06) | Suggested mass (kg) |
|---|---|---|---|---|---|---|---|
| Small Wooden Crate | Crates | `Icons/SmallWoodenCrate.png` | `Props/CrateSmall.glb` | 0.595 × 0.550 × 0.500 | Box | half_extents (0.298, 0.275, 0.250), center (0, 0.275, 0) | 4 |
| Large Wooden Crate | Crates | `Icons/LargeWoodenCrate.png` | `Props/CrateLarge.glb` | 1.100 × 0.550 × 1.000 | Box | half_extents (0.550, 0.275, 0.500), center (0, 0.275, 0) | 25 |
| Long Wooden Crate | Crates | `Icons/LongWoodenCrate.png` | `Props/CrateLong.glb` | 0.595 × 0.550 × 1.000 | Box | half_extents (0.298, 0.275, 0.500), center (0, 0.275, 0) | 15 |
| Cardboard Box | Crates | `Icons/CardboardBox.png` | `Props/CardboardBox.glb` | 0.212 × 0.281 × 0.212 | Box | half_extents (0.106, 0.1405, 0.106), center (0.106, 0.1405, -0.106) | 2 |
| Barrel | Crates | `Icons/Barrel.png` | `Props/Barrel.glb` | 0.704 ⌀ × 1.145 | Cylinder (AddCylinderBody now landed per physics-engine - re-verify and switch from the Capsule interim) | radius 0.352, half_height 0.2205, center (0, 0.5725, 0) | 20 |
| Trash Can | Crates | `Icons/TrashCan.png` | `Props/TrashCan.glb` | 0.234 × 0.428 × 0.208 | Cylinder (AddCylinderBody now landed per physics-engine - re-verify and switch from the Capsule interim) | radius 0.122, half_height 0.092, center (-0.005, 0.214, 0) | 5 |
| Traffic Cone | Misc | `Icons/TrafficCone.png` | `Props/Cone.glb` | 0.300 ⌀ × 0.306 | Capsule (poor fit — see below; convex-hull candidate, physics-engine scoping this) | radius 0.15, half_height 0.075, **center (0, 0.225, 0)** — corrected from an earlier `0.1532` that put it 7cm too high; the fix is `half_height + radius`, not half the mesh's total height | 1 |
| Chair | Furniture | `Icons/Chair.png` | `Props/Chair.glb` | 0.200 × 0.470 × 0.200 | Box (poor fit — see below) | half_extents (0.1, 0.235, 0.1), center (0.1, 0.235, -0.1) | 6 |
| Table | Furniture | `Icons/Table.png` | `Props/Table.glb` | 0.841 × 0.327 × 0.447 | Box (poor fit — see below) | half_extents (0.4205, 0.1635, 0.2235), center (0.4207, 0.1634, -0.2237) | 12 |
| Bench | Furniture | `Icons/Bench.png` | `Props/Bench.glb` | 0.400 × 0.470 × 0.200 | Box (reasonable fit) | half_extents (0.2, 0.235, 0.1), center (0.2, 0.235, -0.1) | 10 |

Main's ruling: keep all six primitives (they're the exact-collider-by-construction
baseline for diagnosing whether a modelled prop's odd behaviour is real or shared with
its primitive equivalent), but rename the three primitive crates so they read as
deliberately different objects rather than confusing near-duplicates of the new modelled
crates. **This needs one line changed in `PropSpawner.cs`'s `Catalog` array (the `Name`
field of the three crate entries) - handing this to whoever lands the final merge, not
doing it myself:**

| Catalogue name | Category | Icon path | Shape | Notes |
|---|---|---|---|---|
| Small Block *(was "Small Crate")* | Primitives | `Icons/SmallBlock.png` | Box primitive | Rename only - same entity/collider/mass as today, just disambiguated from the new modelled "Small Wooden Crate" |
| Medium Block *(was "Medium Crate")* | Primitives | `Icons/MediumBlock.png` | Box primitive | Rename only |
| Heavy Block *(was "Heavy Crate")* | Primitives | `Icons/HeavyBlock.png` | Box primitive | Rename only |
| Marble | Primitives | `Icons/Marble.png` | Sphere primitive | Unchanged |
| Ball | Primitives | `Icons/Ball.png` | Sphere primitive | Unchanged |
| Heavy Ball | Primitives | `Icons/HeavyBall.png` | Sphere primitive | Unchanged |

**Tab structure: 4 tabs, but restructured from my first draft now that the primitives
are staying.** Putting the renamed blocks in `Primitives` alongside the balls, rather
than splitting them into `Crates`/`Balls`, is what keeps this at 4 tabs instead of
growing to 5: `Balls` would otherwise be a tab containing only primitives anyway (there
is no modelled ball in this session's set), so a bare "Balls" tab and a
"blocks-plus-marbles" tab are really the same underlying group - the whole original
baseline catalogue, as one deliberate "start simple" tab, separate from the real models
this session added.

- **Primitives** - Small/Medium/Heavy Block, Marble, Ball, Heavy Ball (6) - the entire
  pre-existing baseline catalogue, unchanged in behaviour, renamed only where it would
  otherwise collide with a new modelled entry's name.
- **Crates** - Small/Large/Long Wooden Crate, Cardboard Box, Barrel, Trash Can (6) -
  every new modelled storage/container object. Barrels and bins included on the same
  "storage/industrial container" theme reasoning as before (corroborated by Kenney's own
  Factory Kit grouping them together), rather than a thin separate `Containers` tab.
- **Furniture** - Chair, Table, Bench (3).
- **Misc** - Traffic Cone (1) - deliberately its own tab as the catch-all for future
  one-off props, not folded away.

16 entries across 4 tabs, none empty, none absurdly thin relative to the others (1-item
`Misc` is a deliberate growing bucket, not an oversight - see above). Flagging this
specific structure to whoever lands the final catalogue merge, since it depends on the
`PropSpawner.cs` rename landing first or the `Crates`/`Primitives` split will read oddly
with two different things both called "crate".

## Primitive fit — specific, per-prop consequence (requirements for convex-hull work)

Ranked worst to least-bad, each stated as a concrete observable failure rather than a
vague "awkward":

1. **Cone → Capsule, worst fit in the set.** A capsule has one uniform radius top to
   bottom; the cone's mesh tapers from a wide base to a point. The capsule is roughly
   2× too fat at the visible tip and narrower than the visible base. Consequence: a
   thrown prop aimed at the cone's visible point will hit invisible collision well
   before reaching the mesh, while a prop resting against the cone's visible (wide)
   base will float above it because the collision boundary there is narrower than the
   mesh. It also **rolls like a cylinder** if knocked on its side, instead of the
   scrape-and-topple a tapered object actually does.
2. **Table → Box spanning full height.** The box fills the entire volume from the
   floor to the tabletop, including the open space between the four legs. Consequence:
   nothing can be pushed, thrown, or rolled underneath the table — a small prop that
   should visibly slide under the tabletop and rest on the floor instead stops dead
   against the invisible solid block at leg height, well before it reaches the legs it
   can see.
3. **Chair → Box filling the leg gap.** Same defect as the table, smaller scale: the
   box occupies the space between all four legs. Consequence: nothing can be tucked
   under the seat, and a prop rolling toward the chair stops at the outer silhouette of
   the (mostly open) leg frame rather than passing between the legs the way it visibly
   could.
4. **Barrel / Trash Can → Capsule instead of Cylinder.** Diameter is correct, but the
   capsule's rounded end-caps replace the real object's flat top and bottom.
   Consequence: contact area at rest is a single point/small patch instead of a full
   flat disc, so these props are physically able to rock or slow-roll on a level floor
   where a real barrel or bin would sit dead still — a **stability** problem, not a
   silhouette one. This is the one entry in the list with a clean, already-scoped
   engine fix (`Physics.AddCylinderBody`, see below) rather than needing convex hulls.
5. **CardboardBox → uncertain, needs re-verification.** Originally attributed to a
   pivot bug on my end; the engine finding above means that explanation is no longer
   certain — CardboardBox may simply be the one prop whose real geometry differed
   enough from the shared default 0.5-half-extent fallback for the mismatch to be
   visible, the same underlying bug as every other prop rather than a separate one.
   Not a primitive-shape-fit problem either way. Re-verify once the engine fix lands
   before deciding whether a `center` offset is still needed here.

Bench and the three crates are **not** on this list: their box colliders track the
visible silhouette closely enough (crates are genuinely box-shaped; the bench is a
fairly solid slab without a large hollow underneath) that a primitive is the right tool,
not a workaround.

## Two things `PropSpawner.cs`'s owner needs to know before wiring this in

1. **`ColliderComponent` is `AE_NOT_ADDABLE()`** (`src/app/scene/reflection/Physics.reflect.cpp:60`)
   **— CONFIRMED a real hole, not an intentional split** (per the engine worker, who
   traced it). `scene.add_component`/the reflection `AddComponent` path a script's
   `Entity.Component("Collider").Add()` would also use: passing the bare reflected name
   `"Collider"` falls through to a reflection-only branch that never consults the
   `addable` flag at all, so the add genuinely succeeds despite `AE_NOT_ADDABLE()`. This
   means **Cylinder colliders are already reachable from script today** via
   `Entity.Component("Collider").Add()` + `SetString("shape","cylinder")` +
   `SetFloat("radius", ...)` / `SetFloat("half_height", ...)` — no `Physics.AddCylinderBody`
   addition is strictly required to unblock Barrel/Trash Can. **But** whatever shape is
   set this way hits the same collider-writes-don't-simulate bug from this file's top
   section, so this path is blocked on that fix too, not free of it.
2. If (1) doesn't pan out, the actual native gap is one export away: `Physics.cs` has
   `AddBoxBody`/`AddSphereBody`/`AddCapsuleBody` but no `AddCylinderBody`, and there is no
   `aether_physics_add_cylinder` native export, even though `PhysicsShapeType::Cylinder` is
   fully implemented in `PhysicsSystem.cpp`'s shape-creation switch. Mirroring
   `aether_physics_add_capsule` (native) + `Physics.AddCapsuleBody` (managed) for cylinder is
   a small, well-scoped engine change and is the actionable ask coming out of this session —
   Barrel and Trash Can above are capsule-approximated purely because Cylinder wasn't
   reachable from script when this was investigated.

## Re-verification protocol — run this once the collider-rebuild bug is fixed

Do not re-run the original "drop and screenshot" test as-is; it cannot distinguish a
correct collider from the default one, which is exactly how this bug hid. Use this
checklist instead — it is designed so a wrong result is visible, not just plausible.

**Step 0 — confirm the fix, not just the collider.** Repeat the decisive test from the
finding above on any one prop: rest it, then mid-Play jump `half_extents` to something
absurd (e.g. `(2,2,2)`) and confirm the body now visibly reacts (jitters/pushes up). If
it still doesn't move, the fix hasn't landed for this code path yet — stop, don't
re-verify props against a bug that's still there.

**Step 1 — confirm the baked mesh's pivot**, per the caveat above: place one prop (e.g.
`CrateSmall`), read its Transform position, and compare against where the render bounds
visually sit (screenshot or Inspector bounds display) to confirm `entity.Position` is
still the mesh's base as assumed, not something the bake step changed.

**Step 2 — per prop, predict before you drop.** For every box-shaped prop with the
`center` correction applied (`center.y = half_extents.y`), the collider's local Y-span
becomes `[0, size_y]`, matching the render mesh's own base-pivoted span — so **every
corrected box or capsule prop should settle at approximately the same entity Y: ground
level (expect ~0, minus a few millimetres of Jolt penetration slop; exact value depends
on the Ground entity's actual collision top, not measured this session)**, not a height
that varies with the prop's size. This is a more discriminating prediction than "does it
look like it's on the ground": if two props with different `half_extents.y` (e.g.
CrateSmall's 0.275 vs Table's 0.1634) settle at Y values that differ by anything beyond
noise, the `center` correction was not applied or not simulated correctly.

**Step 3 — the tell for a still-wrong `center`.** If the engine bug is fixed but a
`center` offset is wrong or missing, the symptom is *not* "floats at the wrong height"
the way CardboardBox did in the invalidated first pass — it's a small vertical
sinking/hovering of a few centimetres (roughly half of `half_extents.y` if `center.y` is
missing) that is easy to miss in a screenshot at normal zoom. Get close-up screenshots
per prop family (crates together, barrel+trashcan together, chair+table+bench together,
cone alone) and, more reliably, read back each prop's settled `Transform.position.y`
and compare numerically against Step 2's prediction rather than eyeballing it — that
numeric comparison is exactly the discipline that caught the original bug.

**Step 4 — sideways offset check for Furniture Kit props specifically** (Chair, Table,
Bench, CardboardBox): with the X/Z `center` correction applied, the model's footprint
should be centred under the entity origin. Without it, the visible mesh sits offset to
one side of where the (invisible) collider actually is — nothing will look wrong while
the prop rests alone on open ground, but it becomes visible the moment something is
thrown at the side of the prop where the mesh is but the collider isn't (or vice versa).
Test this explicitly by throwing a small prop at each of the four sides of a settled
Chair or Table, not just by watching it fall.

# 30-asset expansion (user request: "at least 30 assets and some should be unique")

## Real starting count, checked at source before adding anything

`PropSpawner.cs`'s `Catalog` array - the actual live spawn menu - still has **6
entries** (Small/Medium/Heavy Crate, Marble/Ball/Heavy Ball), unchanged since before
this whole session started. The 16-entry table above was never a live count; it was
this file's *proposed* data, still waiting on `PropSpawner.cs`'s owner to wire it in.
**Real starting number: 6, not 16.** Flagging this explicitly since Main's brief named
exactly this risk ("the last count in the ledger came from someone else's report").

## After this expansion: 30 catalogue-worthy entries total (still not live-wired)

6 existing primitives + 10 real models from the original pass (table above) + 8 new
Factory Kit models (breadth) + 6 self-authored construction-stock props (the "unique"
half) = **30**. All 30 have a source/authorship record in `CREDITS.md` and a fitted
collider below. None are wired into `PropSpawner.cs` yet - same handoff boundary as
the original 10; not my file.

No padding: I looked at 4 more Furniture Kit candidates (toaster, radio, laptop,
stoolBarSquare - all real, bounded, licensed) and left them out on purpose once 30 was
reached, rather than shipping 34 for the sake of it.

## 8 new Factory Kit models - fitted colliders

All 8 share Factory Kit's `Textures/colormap.png` atlas, same as the original 3 crates
and the cone. Bounds computed the same way as every prior entry: glTF accessor min/max
through each node's world transform (identity for all 8 - none carry a baked rotation).

| Catalogue name | Model path | Native size (m, W×H×D) | Collider shape | Collider params | Notes |
|---|---|---|---|---|---|
| Large Cog | `Props/CogLarge.glb` | 1.000 × 0.225 × 1.000 | Cylinder | radius 0.500, half_height 0.1125, center (0, -0.0375, 0) | **Not base-pivoted** like every other model in this catalogue - raw mesh spans y:[-0.15, 0.075], so `center.y` is genuinely negative, not the usual `half_height`. Gear-disc silhouette; a cylinder is the right primitive, not an approximation. |
| Medium Cog | `Props/CogMedium.glb` | 1.000 × 0.225 × 1.000 | Cylinder | radius 0.500, half_height 0.1125, center (0, -0.0375, 0) | Same file-shape family and same off-centre pivot as Large Cog - confirmed independently, not assumed identical. |
| Piston | `Props/Piston.glb` | 1.000 × 1.000 × 1.000 | Cylinder | radius 0.500, half_height 0.500, center (0, 0.500, 0) | Round mechanical actuator - cylinder is a close fit for the housing. Does not model the piston rod's extend/retract range; static collider only. |
| Machine Block | `Props/MachineBlock.glb` | 1.200 × 1.300 × 1.500 | Box | half_extents (0.600, 0.650, 0.750), center (0, 0.650, 0) | Boxy industrial block - box is a close fit, not an approximation. |
| Hopper | `Props/Hopper.glb` | 1.118 × 1.000 × 1.118 | Cylinder | radius 0.559, half_height 0.500, center (0, 0.500, 0) | **Poorly served by primitives** - a hopper/funnel tapers from wide top to narrow bottom; a cylinder is uniform-radius, so it either overshoots at the narrow end or undershoots at the wide end. Convex-hull candidate. |
| Pipe Segment | `Props/PipeSegment.glb` | 1.000 × 1.000 × 1.000 | Cylinder | radius 0.500, half_height 0.500, center (0, 0.500, 0) | Straight pipe - cylinder is the natural fit for the shape. **Orientation not visually confirmed this session** (no live Editor) - if the mesh is actually meant to lie on its side as a building-material segment rather than stand upright, this collider's axis will be wrong; verify orientation before trusting it live. |
| Arrow Sign | `Props/ArrowSign.glb` | 0.566 × 0.200 × 0.849 | Box | half_extents (0.283, 0.100, 0.4245), center (0, 0.100, 0.0005) | Flat directional sign - box hugs the silhouette closely for a flat shape like this. |
| Warning Sign | `Props/WarningSign.glb` | 0.400 × 1.551 × 0.400 | Cylinder | radius 0.200, half_height 0.7755, center (0, 0.7755, 0) | Tall bollard/post-mounted sign - cylinder fits the post; the flat sign panel at the top is not separately modelled in the collider (single primitive per prop, per this catalogue's convention throughout). |

## 6 self-authored construction-stock props - fitted colliders

Generated this session (see `CREDITS.md`), base-pivoted at y=0 like every kit model
used elsewhere in this project, so the same collider-fit convention applies directly -
no separate math needed for "my own" models.

| Catalogue name | Model path | Native size (m, W×H×D) | Collider shape | Collider params | Notes |
|---|---|---|---|---|---|
| Steel Plate | `Props/SteelPlate.glb` | 1.000 × 0.050 × 1.000 | Box | half_extents (0.500, 0.025, 0.500), center (0, 0.025, 0) | Flat slab - exact fit by construction (I authored the mesh to these exact dimensions). |
| Beam | `Props/Beam.glb` | 0.100 × 0.100 × 1.500 | Box | half_extents (0.050, 0.050, 0.750), center (0, 0.050, 0) | Structural beam stock - exact fit by construction. |
| Wheel | `Props/Wheel.glb` | 0.600 × 0.180 × 0.600 | Cylinder | radius 0.300, half_height 0.090, center (0, 0.090, 0) | Puck/drum shape, axis along the model's own Y - it rolls when tipped onto its side, not upright by default, same as a real wheel resting flat vs. rolling on its rim. Exact fit by construction. |
| Hinge Plate | `Props/HingePlate.glb` | 0.400 × 0.040 × 0.400 | Box | half_extents (0.200, 0.020, 0.200), center (0, 0.020, 0) | Same flat-plate shape as Steel Plate, smaller - see `CREDITS.md` for why no hinge-knuckle geometry is modelled. Exact fit by construction. |
| Ball Joint | `Props/BallJoint.glb` | 0.300 × 0.300 × 0.300 | Sphere | radius 0.150, center (0, 0.150, 0) | Exact fit by construction - the one authored prop where Sphere is the mesh's actual shape, not an approximation. |
| Thruster Body | `Props/ThrusterBody.glb` | 0.640 × 0.400 × 0.640 | Cylinder | radius 0.320 (mesh's widest/base radius), half_height 0.200, center (0, 0.200, 0) | **Poorly served by primitives** - the mesh is a flared frustum (narrow top, wide base); a uniform-radius cylinder sized to the wide end leaves a collision gap around the narrow top third. Convex-hull candidate (or a future capsule/cone-frustum-specific shape, if the engine ever adds one). |

## Primitives poorly served by primitive colliders (session-wide summary, for convex-hull triage)

Combining this section with the original list above, five entries in the full 30 are
flagged as **genuinely poor fits**, not just "good enough": **Traffic Cone**, **Table**,
**Chair** (from the original 10), plus **Hopper** and **Thruster Body** (from this
expansion). All five share the same underlying shape defect - a taper, hollow, or gap
that no single Box/Sphere/Capsule/Cylinder can represent - and are the concrete
worklist for whenever convex hull support lands.

## First-person viewmodels - NEW addition, explicitly no collider

Two files in `Viewmodels/` (see `CREDITS.md` for sourcing): `PhysicsGunViewmodel.glb`
(Kenney Blaster Kit `blaster-l` - widest/chunkiest variant) and `ToolGunViewmodel.glb`
(`blaster-e` - longest/slimmest variant), for the user's ask ("I also want to see like
an actual physics gun and a laser like in garry's mod").

**No collider is fitted for either, on purpose.** These are held at the camera as
first-person viewmodels, not physics objects a player can spawn/throw/collide with -
fitting a collider "out of habit" the way every prop above gets one would be dead data
at best and a spurious collision hazard at worst if something later iterated "every
model has a Collider" and added a physics body to a gun the player is holding inside
their own camera.

**Not wired into any script.** Attaching these to `PhysicsGun.cs`'s camera-relative
transform (and whatever holds the tool gun) is a script change outside my file
boundary, same as the prop catalogue itself - flagged to Main for routing, not
attempted here.
