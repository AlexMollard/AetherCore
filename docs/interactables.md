# Interactables

The Sandbox project's Wiremod-style signal graph: sources, sinks and combinators
connected by wires the player drags between them in-game, evaluated by one script
that walks the whole graph on every change. This is a reference for building with
it — adding a device, wiring something up, understanding why a momentary button
can't hold a door open by itself — not a tutorial in dragging wires.

Audience: you have read one or two of `Button.cs`/`Lamp.cs` and are about to add a
third device, or you are building something in `projects/Sandbox` and want to know
what already exists before reaching for a new mechanism. Everything here lives in
`projects/Sandbox/scripts/`.

## The one decision everything else follows from

**Device scripts know nothing about wires.** `Button`, `Lever`, `Lamp`, `Door`,
`Emitter` and `Gate` are each a small, self-contained `EntityScript` with a
handful of plain `public float` fields. None of them import a wire type, look up
`WireHub`, or call anything graph-related — `Button.Interact()` just sets
`Pressed = 1.0f`, `Lamp` just reads `Enable` and writes a light's intensity.
`WireHub` is the *only* class that knows a graph exists at all: it is the one
place that reads a device's output field, writes another device's input field,
and decides what order to do that in.

This is why adding a device is cheap — it is one file with tagged fields, not one
file *plus* a matching change to some shared interface every other device also has
to implement. The cost of that is symmetrical: the two or three small lookup
tables inside `WireHub` and `ToolGun` are the entire integration surface, and
skipping any one of them is a silent no-op, not a compile error (see
[Adding a device](#adding-a-device) below — this is the mistake to not make).

## Devices at a glance

| Script | Role | Fields | Signal shape |
|---|---|---|---|
| `Button` | source | `Pressed` (OUTPUT) | One-frame rising-edge pulse, never a level |
| `Lever` | source | `Out` (OUTPUT) | Level — toggles on `Interact()`, holds until flipped again |
| `Lamp` | sink | `Enable` (INPUT) | Level — drives an existing Point Light's intensity |
| `Door` | sink | `Enable` (INPUT) | Level — drives a Kinematic body's position |
| `Emitter` | sink | `Enable` (INPUT) | Edge-triggered — spawns one instance per rising edge |
| `Gate` | source **and** sink | `A`, `B` (INPUT), `Out` (OUTPUT), `Threshold` | Pure function of `A`/`B`, except `Latch` |

`Gate.Kind` selects the combinator: `And`/`Or`/`Not` (the original three),
`GreaterThan`/`LessThan` (against `Threshold`, ignored by every other `Kind`), and
`Latch` — the odd one out, covered in [Signal shapes](#signal-shapes-and-why-a-button-cant-hold-a-door-open-alone).

## The field convention

A device's inputs and outputs are plain `public float` fields, tagged in an XML
doc comment with `/// <summary>INPUT.</summary>` or `OUTPUT.` — nothing more.
`WireHub` discovers and reads/writes them by name through ordinary reflection
(`System.Reflection.FieldInfo.GetValue`/`SetValue`), not an interface or an
attribute. That is a deliberate choice, not an oversight: an interface would force
every device to implement members it doesn't use (a pure sink has no output to
declare; a pure source has no input), and the field *names* are what both
`WireHub` and `ToolGun` actually key off, which an interface's method shapes
wouldn't give you for free anyway.

**Adding a device means writing a class with tagged `float` fields and nothing
else beyond its own behaviour.** No base class, no interface, no attribute to
apply.

### Adding a device

Writing the class is not the whole job. Two or three lookup tables outside the
device's own file have to know about it, or the wire — or the in-game press
itself — silently does nothing:

- **`WireHub.ResolveScript`** — tries `GetScript<T>()` for every known device type
  and returns the first hit. A device missing here can be wired in the editor
  without error (`ToolGun` doesn't need `ResolveScript` to *create* a wire),
  but `WireHub.Propagate()` will never resolve it as either endpoint, so the wire
  sits there doing nothing forever — no warning, no exception, the value on the
  other end just never changes. This is the exact trap: it happened in this
  codebase when `Door`/`Emitter` were added and `ResolveScript` was forgotten
  first.
- **`ToolGun.OutputFieldOf`** (if the device is a source) and/or
  **`ToolGun.InputFieldOf`** (if it's a sink) — these are what let the wiring
  tool's raycast recognize the device as something it can start or end a wire on
  in the first place. Miss this one and the symptom is different but just as
  quiet: aiming the wire tool at the device and pressing `T` does nothing, no
  flash, no rejection — `TryWireStep` only acts on a hit that resolves to a
  non-null field name.
- **`ToolGun.IsInteractable`/`TryInteract`** — only if the device is meant to
  be pressed or flipped directly in-game, the way `Button` and `Lever` are.
  Wire-driven-only devices (`Lamp`, `Door`, `Emitter`, `Gate`) never need this:
  nothing ever calls `Interact()` on them, so there is nothing here to register.
  A device that DOES define `Interact()` but is missing from `IsInteractable`
  compiles fine and simply never gets pressed — the aim raycast never treats it
  as a target, so `InteractTarget` never becomes it, the `[key] Use` prompt never
  shows over it, and `TryInteract` never has a reason to look for it either.

All three are small, explicit `if (e.GetScript<T>() != null)` branches — see
`WireHub.cs`/`ToolGun.cs` for the shape. There is no compiler check that a new
device registered itself everywhere it needs to; reading `WireHub`'s own file
comment for the reasoning, and grepping for the existing device names across both
files before considering the job done, is the actual safeguard.

## Evaluation semantics

`WireHub` is a single scene entity named `"WireHub"` (found by `Scene.Find`, not a
static registry — see its own file comment on why: a static table would need
cross-peer reconciliation the moment this goes networked, an instance found by
name does not). Every `WireLink` registers itself with it on attach.

- **`Propagate()`** walks every registered wire in topological order — a Kahn's
  algorithm sort (`RebuildOrder`) over the wire graph, rebuilt lazily whenever a
  wire is added or removed. For each wire, in that order, it recomputes the
  source's live output (`RecomputeOutput` — a no-op for everything except `Gate`,
  which calls `Gate.Recompute()`), reads the resulting value, and writes it into
  the target's named input field. Walking in dependency order and recomputing
  *immediately before* each read is what makes a straight three-gate chain settle
  fully in one `Propagate()` call — it does not need three frames, and it does not
  depend on `Gate.OnUpdate` having already run this frame (there is no
  `OnLateUpdate` to guarantee that; there is no `Gate.OnUpdate` at all).
- **`Recompute()` is called by `WireHub`, never by a device's own `OnUpdate`.**
  This is why `Gate` has no `OnUpdate` — its output only ever needs to be current
  at the moment something reads it, and `WireHub` is the only reader.
- **Push, not just poll.** `WireHub.OnUpdate` calls `Propagate()` once a frame as
  a safety net (and to prune dead wires promptly), but `Button.Interact()` also
  calls it directly and synchronously the instant the button is pressed — so a
  press's effects are visible in the same call, not lagged by up to a frame
  waiting for `WireHub`'s own `OnUpdate` to run.
- **Cycles are refused before they can exist**, not detected after the fact.
  `WireHub.WouldCreateCycle(source, target)` does a real reachability walk from
  the *proposed* target back to the proposed source over the *existing* graph,
  and `ToolGun` calls it before ever creating the `WireLink` entity. A
  topological sort has no way to represent a cycle, so this is not an
  optimization — a cycle that made it into `_links` would make `RebuildOrder`
  silently drop it (or every node downstream of it) from `_order` rather than
  hang, which is a worse failure than refusing it at creation.
- **Dead endpoints are pruned, never silently retargeted.** If a wire's source or
  target entity is destroyed, `PruneDead` (called at the top of every
  `Propagate()`) removes the wire and destroys its own marker entity, logging
  once. It checks `World.IsValid(entity)`, not `entity.IsValid` — the latter is
  only `Id != 0`, which stays `true` forever on a stale `Entity` struct that still
  holds an old, now-destroyed id. A pruned wire is never rechecked, so a later
  id-slot reuse can never make it point at something unrelated.

## Signal shapes, and why a Button can't hold a Door open alone

Not every device speaks the same kind of signal, and mismatching them is the
first thing anyone building with this will trip over:

- **`Button.Pressed` is a one-frame rising edge, never a level.** It goes to
  `1.0f` the instant `Interact()` is called and decays back to `0.0f` on the very
  next `OnUpdate` — it is a pulse, not a switch.
- **`Lever.Out` is a level, not a pulse.** `Interact()` flips a persistent on/off
  state and `Out` reads `1.0f`/`0.0f` for as long as that state holds — the
  opposite memory rule from `Button`, on the same kind of player-pressed device.
- **`Lamp.Enable` and `Door.Enable` are levels.** `> 0.5f` reads as "on"/"open";
  `<= 0.5f` reads as "off"/"closed", every single frame, for as long as the wire
  says so.
- **`Emitter.Enable` is edge-triggered**, like `Button`'s output but on the
  *input* side: it tracks the transition from low to high and only acts on the
  rising edge, not the level. Wiring a level source (a plain `Gate`) into it does
  not spawn once per frame the level stays high — only once, on the crossing.

Wire a `Button` straight into a `Door`: the door reads `Enable = 1.0f` for exactly
the one `Propagate()` call the press produced, and `Enable = 0.0f` on every call
after — the door opens for a single frame's worth of motion and then immediately
starts closing again. A momentary source cannot hold a level-reading sink open by
itself; that is not a bug in either device, it is what "momentary" means.

**Two ways to close that gap, and they are not the same tool.** Wire a `Lever`
straight into a `Door`'s `Enable` instead of a `Button`: one entity, one wire, the
door stays open exactly as long as the lever is flipped on, and closes the moment
it's flipped off — this is the answer for "I want a switch," full stop. `Gate.Kind
= Latch` is for when the level still has to be *derived* from something
momentary you don't control directly — `A` sets, `B` resets, neither holds the
last `Out`, so it turns a `Button`'s (or any other pulse source's) one-frame
pulse into a level with its own independent Set and Reset inputs. Reach for
`Latch` when you need Set and Reset to be two different events; reach for `Lever`
when you just need a switch a player flips by hand — using a `Latch` for the
second case works, but costs an extra entity and wire to say something a `Lever`
says on its own.

## The kinematic rule for Door

`Door` needs to move a real physics body that other props may be resting against
— open a door with a crate leaning on it, and the door has to shove the crate, not
pass through it. That rules out the two obvious approaches:

- **A `Static` body never moves at all** — `Enable` could still change, but the
  door's collider would stay exactly where it was created forever.
- **A plain `Entity.Position` teleport imparts essentially zero momentum into
  whatever it passes through.** This was measured directly in this engine: moving
  a resting body's transform by teleport rather than through the physics solver
  left a residual momentum on a body it swept through of `-2.3e-11` — not "small,"
  the physics equivalent of zero. A door that teleported through a crate would
  visibly clip through it instead of pushing it aside.

The actual mechanism is `PhysicsSystem::PushKinematicTargets`
(`PhysicsSystem.cpp`, called once per physics step, before the Jolt solve): for
every body whose `RigidBodyComponent.motionType` is `Kinematic`, it reads that
entity's *current* `TransformComponent` and calls Jolt's own
`BodyInterface::MoveKinematic`, which computes the velocity needed to carry the
body from where Jolt currently has it to the authored transform over exactly one
physics step — a real, solved velocity, not a placement. Do that every step with
a transform that has moved a little further each time, and the body genuinely
pushes whatever it's in contact with, the same way a network-replicated crate's
position writes make it shove other props out of its way.

`Door.OnAttach` requires an entity that already carries a `Collider` and a `Rigid
Body` (any shape — it warns and does nothing if either is missing, the same
"drives an existing X, does not create one" convention `Lamp` uses for Point
Light) and calls `Physics.SetMotionType(Self, PhysicsMotionType.Kinematic)` on it.
`Door.OnUpdate` then steps `Self.Position` toward the open or closed target by
`Speed * deltaTime` each frame — never snapping straight there — because that
incremental write, read back by `PushKinematicTargets` on the very next physics
step, *is* the velocity-based motion described above.

`Physics.SetMotionType` (`Physics.cs`/`PhysicsExports.cpp`) is a general SDK
export, not specific to `Door` — the C++ method it wraps
(`PhysicsSystem::SetBodyMotionType`) already existed for `NetworkContext`'s own
authority handover, but nothing surfaced it to script before this. Anything else
that needs a script-driven Kinematic body can call it the same way.

## Using the wiring tool in game

`ToolGun` is attached to the Main Camera alongside `PhysicsGun`/`SpawnMenu`/
`PropSpawner`, and shares their forward-ray-from-eye-height aim. Wiring is one
of its modes (`ToolGun.ToolMode.Wire`) - Light/Colour/Remove are the others,
cycled with the mouse wheel; see the tool gun's own file header for the full
mode list and why Weld/Rope aren't in it yet.

- **`G` (`ToolGun.InteractKey`) presses whatever `Button` is directly ahead, or
  flips whatever `Lever` is directly ahead.** This is ordinary gameplay use and
  has nothing to do with wiring — it is the same raycast, reused, calling
  `Interact()` on whichever of the two it hit.
- **`T` (`ToolGun.ToolFireKey`) fires the current mode's action - in `Wire` mode,
  a two-press flow:**
  1. The first press on an entity exposing an output (`Button`, `Lever` or
     `Gate`) marks it pending and tints its material `PendingTint` (a warm
     yellow by default) — the same held-prop emissive-tint idiom `PhysicsGun`
     already uses, so there is only one visual convention in this game for
     "something is being tracked for you." Pressing `T` on that same pending
     entity again cancels quietly (tint clears, nothing else happens).
  2. The second press on a different entity exposing a compatible input
     (`Lamp`/`Door`/`Emitter`'s `Enable`, or a `Gate`'s `A`/`B` — whichever of the
     two is not already wired) completes the connection: a new `WireLink` marker
     entity is created and its four fields are populated once its script
     instance actually exists (`AddScript` only queues the attach; it doesn't
     happen the same frame). **The wire is visible** — a thin stretched cube runs
     between the two endpoints from the moment the connection completes, dim
     grey while its carried value reads at or below 0.5 and warm yellow above
     it, so a signal that's live reads at a glance without opening anything.
     `WireHub` keeps it positioned every frame (endpoints move — a prop gets
     physics-gunned, a `Door` slides) independently of how often the signal
     itself actually changes, and destroys it the instant the wire it belongs
     to does (see `WireLink`'s own file comment on why that needs an explicit
     call rather than following from parenting alone).
  3. A press that would close a cycle (checked via `WireHub.WouldCreateCycle`
     before the wire is created) — or that lands on an entity with no compatible
     input at all — is rejected outright: the target flashes `RejectTint` (red by
     default) for `RejectFlashSeconds`, and the pending source is cleared. A
     rejection is always visible, never silent.
- **The `[G] Use` HUD prompt** (`UiHud`'s `_interactLabel`, below the crosshair)
  shows exactly when `ToolGun.InteractTarget` is a valid entity — aiming
  directly at a `Button` or `Lever` within `ToolGun.MaxRange` — and is built
  from `InteractKey` itself (`InteractPromptText`), so it can never show a stale
  key if the binding ever changes. It clears the instant the raycast stops
  hitting one of those two, whether from looking away or stepping out of range;
  both collapse to the same `InteractTarget` check, since `MaxRange` already
  bounds the raycast that produces it.

## Persistence

Wires survive scene edits now. `WireLink.Source`/`Target` used to resolve by
positional index into the scene's entity array, which a hand-inserted or
reordered entity would silently repoint at the wrong thing with no error — a
real defect this session, now fixed at the serializer level. Verified live:

- A wire saves with its endpoints carrying both a legacy positional index and a
  stable node id. On load, the stable id wins. Confirmed by hand-inserting an
  entity at array position 0 after saving a Button → Lamp wire: the wire text
  was untouched on disk, and both endpoints still resolved to the correct
  entities after reload, where a positional-only reference would have shifted.
- A file saved before this fix (positional index only, no stable id) still
  loads correctly — the positional path is a working fallback, not a dead one.
- An Entity-typed property that was never wired saves as explicitly unset and
  comes back unset on load, rather than silently binding to whatever entity
  happens to sit at index 0.
- The wire's visible segment is never itself part of any of this — it is a
  separate, transient child entity `WireLink` creates fresh in its own
  `OnAttach` every time (including on load), from whatever `Source`/`Target`
  currently resolve to. Nothing about its position, colour or even its
  existence is persisted; it is derived data, regenerated the same way
  every time rather than baked into the scene file.

**The one behavior most likely to surprise anyone using this: a wire is only
captured into the save file if the scene is saved *while the game is
playing*.** `Source`/`Target` are live C# fields on a running `WireLink`
instance, and the mechanism that pulls a script's current field values into
the persisted save data only runs against a live instance. Build something in
Play, stop without saving, and the wire is gone — connect it, THEN save while
still in Play, not after stopping.

A wire whose endpoint got deleted behaves the same at load time as it does at
runtime (see `WireHub`'s dead-endpoint pruning above): it logs
`WireHub: dropping wire '' -> '' - an endpoint was destroyed.` and disappears.
If you delete something a wire was attached to and the wire seems to have
vanished with no error, that line in the log is where it went.

## Live verification script

Everything above is claimed on code evidence only — grep, static reads, offline
builds. Nothing in this file has been seen rendering on screen. This pass
covers every item the phase has queued, split into what runs through the
control protocol with no keypress at all, what runs through it via synthetic
key/mouse input pending live confirmation, and what is still genuinely out of
reach (a real, unlocked, focused session).

**Why the split, and how it changed.** `docs/mcp-setup.md` originally treated
all focus-gated input as unreachable from an agent session. `ui-verify` traced
the actual input paths in source and narrowed that considerably:
`engine.send_input`'s `mouse_pos`/`mouse_down`/`key_down` write straight into
`Input`'s own synthetic-state fields (`Input.hpp:456-469`), and at
`Input.cpp:159` the synthetic half is OR'd into `IsKeyPressed`/
`IsMouseButtonDown` **without** the `focused` gate the real-GLFW half carries.
Those are exactly the calls `[G] Use`, `ToolFireKey`, and every other
in-game keybind read — so a synthetic press is indistinguishable from a real
one to any of this file's scripts, no OS focus required. Genuinely still
blocked: anything gated on `windowFocused` itself (cursor lock, the
Escape-release hatch — not part of this checklist) and, separately,
**mouse-look aiming** — the camera's facing comes from cursor-locked relative
deltas, which an agent cannot produce. The substitute is not mouse-look, it is
`set_transform` on the camera/player entity: point it at a device directly
through the control protocol, then fire the key synthetically. **Status:
read-verified, not yet confirmed live** — `ui-verify` is proving it with a
real click the moment the Editor is free; nothing below promotes out of
"pending confirmation" until that lands.

The two harder gaps found while looking for a way around the old focus
assumption still stand on their own, independent of synthetic input:

- **`set_component`/`get_component` cannot reach a script's fields at all.**
  Both resolve `type` through `reflect::FindComponentType` (the native
  `AE_COMPONENT` catalog — Point Light, Rigid Body, Skinned Mesh, 28 entries
  total) and, failing that, `editor::FindComponentFields`, whose entire table is
  one entry: `"Material"` (`ComponentFields.cpp`). `Button`, `Lever`, `Gate`,
  `Lamp`, `Door` and `Emitter` are C# scripts, not `AE_COMPONENT`s — their
  `Pressed`/`Out`/`A`,`B`/`Enable` fields are found by `WireHub`'s own
  `System.Reflection.FieldInfo` reflection, invisible to both native catalogs.
  There is no way to write, or read back the LIVE value of, one of these
  fields on an already-attached script through the control protocol —
  `list_scripts` only echoes the property cache `add_script` seeded at
  creation, which nothing keeps in sync with the running instance.
- **A brand-new `WireLink` still cannot be constructed field-by-field through
  `add_script`** — its `properties` parameter parser (`ControlMethods2D.cpp`)
  only recognizes bool, int, float, string and `[x,y,z]`, no `Entity` case, and
  `Source`/`Target` are `Entity` fields. This no longer matters for *creating*
  a wire, though: `ToolGun`'s own two-press `T` flow does the linking in C#,
  never through `add_script`'s property parser, so a synthetic `T` press (aimed
  via `set_transform`) creates a real wire the same way a human's would. It
  only matters if something ever needs to fabricate a `WireLink` with exact
  field values *without* going through the tool at all.

Net effect: nothing observable in this checklist is out of reach in
principle any more. What is still true is that every check below whose
trigger is a keypress is **pending `ui-verify`'s live confirmation** of the
synthetic-input path, not yet a plain pass.

**Setup.** `Sandbox.scene.toml` already has `Test Button`, `Test Gate`, `Test
Lamp` placed and wired (their wires were authored by a human through
`ToolGun`, which is the only way any wire in this project has ever been
created). It does **not** yet have a `Test Lever`, `Test Door` or `Test
Emitter` — see [Placing the three missing fixtures](#placing-the-three-missing-fixtures)
below for the exact call sequence. **Capture method: `capture_texture` on
`PostProcess.FinalColor`, never `screenshot`.** `ui-verify` proved live that
the Viewport panel draws editor-only overlays (gizmos, seams) that do not
exist in the actual rendered frame — `screenshot` risks chasing an artefact, or
worse, passing a wire that only looks right because an editor overlay is
drawing over it. `capture_texture(PostProcess.FinalColor)` is the composited,
chrome-free frame every check below should be judged against.

### Agent-runnable now

A. **Sink reaction to a directly-seeded input, no wire involved.** `add_script`
   a fresh `Lamp` with `properties: {"Enable": 1.0}` — this seeds the field at
   creation, bypassing `WireHub` entirely. Expected: its Point Light reads at
   `OnIntensity`, confirmed via `get_component("Point Light")`. Recreate with
   `Enable: 0.0`: expected `OffIntensity`. This proves the sink's own
   `OnUpdate` read, not the wire path. — PASS / FAIL:
B. **Door's Kinematic takeover, no wire involved.** Place a `Door` per the
   sequence below with `Enable` seeded at creation. Over several frames,
   `get_component("Rigid Body")` should read back `motion = "kinematic"`
   (flipped from whatever it was added with) the instant `Door.OnAttach` runs,
   and repeated `get_entity` position reads should show it stepping smoothly
   toward its target, never jumping there in one frame. — PASS / FAIL:
C. **Emitter is edge-triggered, not level-triggered, no wire involved.**
   `add_script` a fresh `Emitter` with `properties: {"Enable": 1.0}` — already
   high at creation, so it never saw a rising *edge*. Expected: `list_entities`
   shows **zero** spawned instances after several frames, proving it reacts to
   the crossing, not the level. A true press-triggered spawn needs a real
   `Interact()` call upstream — see F/H below. — PASS / FAIL:
D. **Six device models, placement only.** After running the sequence below,
   `capture_texture(PostProcess.FinalColor)` from a few angles per device.
   Expected: each is visible at its authored position, right-side up, not
   floating above or embedded in the floor, and at a plausible scale (flag any
   that read 100× off — likely the centimetres-vs-metres `add_model` scale
   note). This checks placement only, not whether the raycast hits it — that
   half is pending live confirmation, see I below. — PASS / FAIL:

### Agent-runnable, pending `ui-verify`'s live confirmation

Every item here is triggered by a synthetic key press (`engine.send_input`'s
`key_down`/`key_up`) after aiming by moving the camera/player entity with
`set_transform` — never by trying to reproduce mouse-look, which stays out of
reach. None of these have been run live yet; treat every PASS/FAIL line below
as unconfirmed until `ui-verify` reports back on the actual mechanism.

E. **`[G] Use` prompt, range and look-away.** `set_transform` the player/camera
   to face `Test Button` from just inside `ToolGun.MaxRange`, capture
   `PostProcess.FinalColor` and look for the prompt text near the crosshair;
   `set_transform` it just outside range (or facing away) and confirm the
   prompt is gone in the next capture. No `send_input` needed for this one —
   it is driven purely by `InteractTarget`'s per-frame raycast, not a keypress.
   — PASS / FAIL:
F. **Button vs Lever, momentary vs latching, and the wire visual end to end.**
   Aim at `Test Button` via `set_transform`, send a synthetic `key_down`/
   `key_up` for `G`, and capture on the next frame: expect a single-frame
   pulse effect that has reverted by the frame after. Aim at the placed
   `Lever` the same way and press `G` once: expect it snaps on and holds
   across several subsequent captures. Aim at `Test Button` (or `Lever`) and
   send two `T` presses — first on the source, then re-aimed at `Test Lamp` —
   to create a real wire the same way `ToolGun`'s C# does the linking
   itself, not through `add_script`'s property parser (which still cannot
   hold an `Entity` field, but no longer needs to). Confirm the wire's visible
   segment: real endpoints, correct orientation from a couple of angles, dim
   grey ≤0.5 / warm yellow >0.5 in the same frame the value crosses, follows
   an endpoint moved with `set_transform`, and vanishes the instant the wire
   entity is deleted. All against `PostProcess.FinalColor`. There is no
   separate `Switch` script in this codebase — `Lever` is the switch. —
   PASS / FAIL:
G. **Gate truth table and Door/Emitter as real sinks, on a live wire.** Build
   the wires the same way as F (aim via `set_transform`, connect via two
   synthetic `T` presses) — `Gate.Recompute()` only ever runs as a side effect
   of `WireHub.Propagate()` walking a real wire with that `Gate` as source, so
   this genuinely needs one, not just the ability to press a key. Drive `A`/
   `B` with two `Lever`s, read `Out` off a `Lamp`, confirm all four
   combinations for whichever `Kind` `Test Gate` holds, then repeat for
   `GreaterThan`/`LessThan`/`Threshold` and `Latch` (Set/Reset on two
   different `Lever`s; confirm `Out` holds after Set returns low). Wire a
   `Lever` into the placed `Door` and `Emitter`'s `Enable` the same way and
   confirm the open/close glide and the one-spawn-per-press behaviour live. —
   PASS / FAIL:
H. **Lever handle animation.** Aim at the placed `Lever` via `set_transform`
   and send a synthetic `G` press. Expected: the handle visibly swings through
   its hinge arc over roughly the clip's authored duration, not an instant
   snap, and holds the new position rather than snapping back or looping. The
   wire fed by its `Out` should flip grey/yellow in the same frame the swing
   starts, not after it finishes. Send `G` again immediately: the return
   swing should play from the handle's current position, not glitch-snap
   through it. — PASS / FAIL:
I. **Six device models, raycast hit.** `set_transform` the camera to face
   `Button` and `Lever` specifically (not just anywhere near them) and confirm
   `[G] Use` actually appears in a `PostProcess.FinalColor` capture — a
   collider that doesn't match its visual mesh would show the prompt over
   empty air or fail to show it while facing straight at the device. —
   PASS / FAIL:

### Needs a human

Nothing in this specific checklist is left here. The general limitation still
stands for other features — anything gated on `windowFocused` itself (cursor
lock, the Escape-release hatch, `docs/mcp-setup.md`'s own examples) has no
synthetic-input substitute, because those paths check the real GLFW focus
callback directly rather than reading through `IsKeyPressed`/
`IsMouseButtonDown`. None of the devices, wires or combinators above touch
that flag.

### Placing the three missing fixtures

`Test Button`/`Test Lamp` establish the convention this follows exactly: a
static `Rigid Body` + a shape-matched `Collider` (added via `add_component`'s
`"Box Collider"` catalog entry — the bare `"Collider"` type name is not
directly addable, only its shape-specific catalog entries are), sized to the
mesh, then the device script attached last so its `OnAttach` sees physics
already in place. Extends the existing row (`Test Button` at x=-3, `Test Gate`
at x=0, `Test Lamp` at x=3, all at y=1.6, z=11.0).

Model paths are `project://assets/models/Devices/{Lever,Door,Emitter}.glb`.
**Scale is a guess, not a measurement** — no Editor session was available to
open these files, so verify with `capture_texture(PostProcess.FinalColor)`
after step 1 of each device and correct `scale` (skinned models authored in
centimetres commonly want ~0.01, per `add_model`'s own doc) before continuing.
Collider `half_extents`/`center` below are starting guesses for the same
reason — resize with `set_component` once the model's actual bounds are
visible.

**Lever** (x = -6.0):
1. `add_model {"path": "project://assets/models/Devices/Lever.glb", "name": "Test Lever", "position": [-6.0, 1.6, 11.0], "scale": [1.0, 1.0, 1.0]}` → note the returned root `id` as `<leverId>`.
2. `add_component {"id": "<leverId>", "type": "Box Collider"}`
3. `set_component {"id": "<leverId>", "type": "Collider", "values": {"half_extents": [0.25, 0.35, 0.15]}}`
4. `add_component {"id": "<leverId>", "type": "Rigid Body"}` (defaults to static-suitable; `Lever` never changes its motion type, unlike `Door`)
5. `set_component {"id": "<leverId>", "type": "Rigid Body", "values": {"motion": "static"}}`
6. `add_script {"id": "<leverId>", "type": "Lever"}`

**Door** (x = 6.0, base assumed at floor height — verify):
1. `add_model {"path": "project://assets/models/Devices/Door.glb", "name": "Test Door", "position": [6.0, 0.0, 11.0], "scale": [1.0, 1.0, 1.0]}` → `<doorId>`.
2. `add_component {"id": "<doorId>", "type": "Box Collider"}`
3. `set_component {"id": "<doorId>", "type": "Collider", "values": {"half_extents": [0.5, 1.0, 0.1]}}`
4. `add_component {"id": "<doorId>", "type": "Rigid Body"}` — leave at its default motion type; `Door.OnAttach` calls `Physics.SetMotionType(Self, Kinematic)` itself, and needs to find the `Rigid Body` already present, not pre-set to any particular mode.
5. `add_script {"id": "<doorId>", "type": "Door"}`

**Emitter** (x = 9.0, with clear floor toward the room's centre for spawned instances to land):
1. `add_model {"path": "project://assets/models/Devices/Emitter.glb", "name": "Test Emitter", "position": [9.0, 1.6, 11.0], "scale": [1.0, 1.0, 1.0]}` → `<emitterId>`.
2. `add_component {"id": "<emitterId>", "type": "Box Collider"}`
3. `set_component {"id": "<emitterId>", "type": "Collider", "values": {"half_extents": [0.3, 0.3, 0.3]}}`
4. `add_component {"id": "<emitterId>", "type": "Rigid Body"}`
5. `set_component {"id": "<emitterId>", "type": "Rigid Body", "values": {"motion": "static"}}`
6. `add_script {"id": "<emitterId>", "type": "Emitter"}`

`save_scene` once all three are placed and visually verified, so the fixtures
persist for whoever runs the checklist above.

## Where to go next

| You want | Look at |
|---|---|
| The evaluator and its cycle/pruning guarantees | `WireHub.cs` |
| The two-or-three device-registration lookup tables | `WireHub.ResolveScript`, `ToolGun.OutputFieldOf`/`InputFieldOf`, `ToolGun.IsInteractable`/`TryInteract` |
| A worked source/sink/combinator to copy from | `Button.cs`/`Lever.cs` (source), `Lamp.cs` (sink), `Gate.cs` (combinator) |
| The Kinematic-body mechanism in the engine itself | `src/engine/physics/PhysicsSystem.cpp` — `PushKinematicTargets`, `SetBodyMotionType` |
| The reflection-driven generic component access `Lamp`/`Door` use | `managed/AetherCore/Component.cs` (`ComponentAccess`) |
| The raycast/range/tint idiom every interactable shares | `ToolGun.cs`, `PhysicsGun.cs`, `Beam.cs` |
