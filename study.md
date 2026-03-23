# Rendering Interview Study Sheet

## Use This Sheet Like Flash Cards
1. Read one section only.
2. Say the audio cue out loud.
3. Move to the next section.

---

## 1. Vulkan Synchronization

### Core idea
Synchronization is about hazards, not "add barriers everywhere".

### Three-question check
1. What wrote the resource before?
2. What reads or writes it next?
3. Are those accesses ordered and visible?

### Fast example
1. Compute writes indirect args.
2. Graphics does indirect draw.
3. Missing barrier -> intermittent missing draws.
4. Fix -> compute-write to indirect-read barrier.

### OOP correlation
1. Producer pass = mutating method.
2. Consumer pass = reading method.
3. Barrier = explicit happens-before edge.

### Audio cue
"Producer, consumer, hazard, then barrier."

### Interview sentence
I identify producer, consumer, and access type first, then choose minimal stage/access synchronization to cover that exact hazard.

---

## 2. Buffers Vs Images

### Core idea
1. Buffers: memory visibility/order barriers.
2. Images: visibility/order plus layout transitions.

### Fast example
1. Compute writes list buffer -> next pass reads list buffer.
2. Use buffer memory barrier, no layout transition.
3. Color target written then sampled in post -> needs image layout transition.

### OOP correlation
1. Buffer = plain mutable field.
2. Image = field plus state machine (layout state).

### Audio cue
"Buffers need memory barriers. Images need barriers plus layout."

### Interview sentence
If the resource is a buffer I discuss memory hazards only, if it is an image I include both memory visibility and required layout state.

---

## 3. Memory Types And Coherency

### Core idea
Pick memory by intent.

### Practical mapping
1. Static vertex/index/material data -> device-local.
2. Dynamic per-frame upload -> host-visible mapped ring.
3. Readback -> host-visible read path (host-cached if available).

### Fast example
1. CPU writes mapped constants.
2. Non-coherent memory.
3. Skip flush -> old transforms appear.
4. Flush written range -> stable output.

### OOP correlation
1. Device-local = private field optimized for internal use.
2. Upload buffer = constructor parameter staging area.
3. Readback = debug getter path.

### Audio cue
"Static device-local, upload host-visible, readback host-visible read path."

### Interview sentence
Even with VMA I still encode intent clearly: GPU-only static data, CPU-to-GPU upload data, and GPU-to-CPU readback data.

---

## 4. Bindless Done Safely

### Core idea
Bindless is a large shader-indexed descriptor table shared across many draws.

### Main risks
1. Stale descriptor indices.
2. Reusing a slot while GPU still references it.

### Fast example
1. Texture A freed.
2. Same slot reused immediately for Texture B.
3. Random meshes show Texture B incorrectly.
4. Fix -> stable handle + generation + deferred slot recycle.

### OOP correlation
1. Descriptor slot = private array index.
2. Public texture handle = object ID.
3. Generation = versioned handle validation.
4. Deferred free = GC queue with safe epoch.

### Audio cue
"Never trust raw slot IDs."

### Interview sentence
I use stable logical handles with generation/version checks and defer slot reuse until GPU completion to prevent stale-sample bugs.

---

## 5. Render Graph / Frame Graph

### Core idea
Passes declare reads/writes; compiler derives order, sync, lifetimes, and transient allocation.

### Aliasing rule
Resources can alias if lifetimes do not overlap and physical requirements are compatible.

### Fast example
1. Shadow temp target used early.
2. Bloom temp target used later.
3. No overlap -> one physical allocation reused.
4. If overlap exists -> no alias allowed.

### OOP correlation
1. VirtualResource = interface/handle.
2. PhysicalResource = pooled implementation instance.
3. GraphCompiler = dependency orchestrator.

### Audio cue
"No overlap, then alias."

### Interview sentence
A frame graph is a dependency compiler for rendering: it turns declared pass intent into safe execution and efficient memory reuse.

---

## 6. GPU-Driven Rendering

### Core idea
GPU decides what to draw by culling and generating indirect work, instead of CPU issuing every draw.

### Frame sequence
1. CPU uploads scene metadata.
2. Compute culls and compacts visible instances.
3. Compute writes indirect args.
4. Sync compute output for graphics read.
5. Graphics executes indirect draws.

### Fast example
1. 200k instances.
2. Compute keeps 18k visible.
3. Graphics draws only those via indirect args.

### OOP correlation
1. CPU submits job list object.
2. GPU compute mutates it into visible-job list.
3. Graphics consumes same list object.

### Audio cue
"Cull, compact, indirect args, barrier, draw."

### Interview sentence
Indirect draws are not just batching; they are the execution bridge from GPU visibility results to final rendering.

---

## 7. Timeline Semaphores vs Binary vs Fence

### Core idea
1. Binary semaphore: one-shot signal/wait.
2. Fence: mainly GPU->CPU completion.
3. Timeline semaphore: monotonic counter for richer dependency tracking.

### Fast example
1. Upload ring allocation tagged with timeline value 420.
2. Reuse only when timeline >= 420.

### OOP correlation
1. Binary = bool flag.
2. Fence = one future completion handle.
3. Timeline = increasing sequence number.

### Audio cue
"Timeline gives me safe reuse epochs."

### Interview sentence
Timeline semaphores simplify multi-queue and frame-resource lifetime tracking by replacing many one-off sync objects with value-based waits/signals.

---

## 8. Pipeline Permutations

### Core idea
Permutation explosion is a combinatorial content/state problem.

### Why it still exists
1. Material feature combinations.
2. Render target/state combinations.
3. Shader quality tiers and variants.

### Fast example
1. Features: clearcoat x skinning x alpha mode x shadow mode.
2. Combinations grow fast.
3. Compile time and runtime PSO cache pressure explode.

### OOP correlation
1. Uber shader = giant base class with many branches.
2. Specializations = focused subclasses with smaller behavior surface.

### Audio cue
"Limit static combinations, cache aggressively."

### Interview sentence
I reduce permutation cost by constraining static feature space, pushing some options to dynamic data, and aggressively caching/deduplicating PSOs.

---

## 9. Mesh Shaders Vs Traditional Indirect Path

### Core idea
Mesh shaders are a powerful optional path, not always the only path for a cross-platform renderer.

### Fast comparison
1. Mesh path: meshlet preprocessing, strong fine-grained culling potential.
2. Traditional path: mature tooling and broad portability with compute+indirect.

### OOP correlation
1. Mesh path = specialized strategy implementation.
2. Traditional path = default strategy implementation.

### Audio cue
"Use mesh shaders where they win, keep robust fallback."

### Interview sentence
I treat mesh shaders as a hardware-optimized strategy and keep a strong compute-plus-indirect fallback for portability and predictable behavior.

---

## 10. Answer Structure Under Pressure

### 3-step answer shape
1. One-line definition.
2. Concrete frame example.
3. Key tradeoff or failure mode.

### If unsure, use this script
1. "My assumption is..."
2. "I would verify by..."
3. "Likely hazards are..."
4. "If wrong, I pivot to..."

### OOP correlation
1. Assumption = interface contract.
2. Verification = unit test for contract.
3. Pivot = swap implementation strategy.

### Audio cue
"Assume, verify, narrow."

### Avoid list
1. Bluffing exact details.
2. Repeating apologies.
3. Defending weak answers too long.
4. "I will add barriers everywhere."

---

## 11. Your Renderer Roadmap Answer (Use This)

### 30-second version
1. Add per-frame upload/ring allocator first.
2. Move passes to declared read/write metadata.
3. Add graph compiler for sync and lifetime.
4. Add transient aliasing once validation is trustworthy.
5. Add GPU culling + indirect submission.
6. Keep bindless handles versioned and safely recycled.

### OOP correlation
1. Stepwise refactor from manual orchestration to compiler-managed orchestration.
2. Convert implicit behavior to explicit contracts.

### Audio cue
"Incremental architecture wins over big-bang rewrite."

### Interview sentence
I would evolve the renderer incrementally: first deterministic frame-data lifetimes, then declarative pass contracts, then compiler-managed sync/allocation, then GPU-driven scaling.

---

## 12. One-Page Memory Hooks

1. Hazard rule: who wrote, who reads, is it visible/ordered?
2. Memory rule: static device-local, upload host-visible, readback host-visible read path.
3. Aliasing rule: no lifetime overlap + compatible requirements.
4. Bindless rule: stable handle + generation + deferred recycle.
5. GPU-driven rule: cull -> compact -> indirect args -> barrier -> draw.

Final audio line:
"Hazard first, API second."