options gen2;

require world;
require math;

// ── Entity script example ────────────────────────────────────────────────────
// Attach via the inspector (Add Component -> Script - spinner.das), the
// outliner, or das add_script(world, e, "entities/spinner.das").
//
// Entity scripts export:
//   on_entity_attach(world, self)      - once, on the first play tick
//   on_entity_update(world, self, dt)  - every play tick
//
// They are frozen while the editor is paused, restart after Stop restores
// the scene, and recompile on F5. One compiled program is SHARED by every
// entity using the script - keep per-entity state in components (get/set
// through bindings), not in script globals.

[export]
def on_entity_attach(world : World?; self : uint)
{
    print("spinner: entity script attached\n");
}

[export]
def on_entity_update(world : World?; self : uint; dt : float)
{
    var e = get_euler(world, self);
    set_euler(world, self, float3(e.x, e.y + 45.0 * dt, e.z));
}
