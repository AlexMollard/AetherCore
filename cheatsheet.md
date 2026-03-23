# CHEAT SHEET — Scan, Don't Read

---

## SYNC  ← if they say "barrier" or "hazard"

```
WHO WROTE  →  WHO READS  →  are they ordered + visible?
```

| thing       | barrier type              |
|-------------|---------------------------|
| buffer      | memory barrier            |
| image       | memory barrier + layout   |

**srcStage** = where the write happens  
**dstStage** = where the read happens  
**srcAccess** = what kind of write  
**dstAccess** = what kind of read  

> "I find the producer, the consumer, then pick masks that cover that hazard."

---

## MEMORY TYPES  ← if they say "allocation" or "memory"

| use case     | type                         |
|--------------|------------------------------|
| static mesh  | device-local                 |
| upload/frame | host-visible (ring)          |
| readback     | host-visible + cached        |

> "Static stays on GPU. Dynamic uploads per frame. Readback is host-visible."

---

## BINDLESS  ← if they say "descriptor" or "bindless"

```
big table  →  material stores INDEX  →  shader fetches by index
```

**Safe = 3 things:**  handle + generation counter + deferred recycle  
*(recycle only after GPU finished with that slot)*

> "Stable handle, generation to catch stale refs, deferred free."

---

## RENDER GRAPH  ← if they say "frame graph" or "render graph"

```
passes declare reads/writes
      ↓
compiler derives: order → barriers → lifetimes → aliasing
```

**Aliasing** = two transient resources, never live together → share same memory  

> "Passes declare intent, the graph figures out sync and memory."

---

## GPU-DRIVEN  ← if they say "indirect" or "GPU-driven"

```
compute cull  →  compact  →  write indirect args  →  barrier  →  vkCmdDrawIndexedIndirectCount
```

> "GPU culls, writes its own draw args, CPU just fires one dispatch."

---

## TIMELINE SEMAPHORE  ← if they say "semaphore" or "sync objects"

| type      | signal          | use case              |
|-----------|-----------------|-----------------------|
| binary    | one-shot        | present/acquire       |
| fence     | GPU → CPU       | wait on CPU side      |
| timeline  | monotonic int   | frame-resource reuse  |

> "Timeline replaces a pile of fences with one counter per queue."

---

## PIPELINE PERMUTATIONS  ← if they say "PSO" or "shader variants"

**Problem:** combinations explode (MSAA x alpha x topology x ...)  
**Fix:**
- limit static feature combos
- PSO cache everything
- standardize pass families

> "I treat it like a content problem. Reduce combos, cache hard, standardize formats."

---

## MESH SHADERS  ← if they say "meshlet" or "mesh shader"

```
Task shader (cull meshlets)  →  Mesh shader (emit verts)  →  Fragment
```

Fallback when not supported = compute cull + indirect draw

> "Task culls, mesh emits. Same GPU-driven idea, just per-meshlet."

---

## IF YOU BLANK

> "My assumption is [X]. I'd verify by [Y]. Failure modes are probably [Z]."

> "That's fair — better framing is..."

> "Let me think out loud for a second."

---

## NEVER SAY

- "I'd just add barriers everywhere"
- "Bindless means no descriptors ever"
- "I'd use one uber shader"
- "I don't know" ← say "I'm not sure, but I'd imagine it's something like X and Y because of Z"
