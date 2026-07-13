#pragma once

#include <functional>
#include <string>

namespace aether::app::scene
{
	// Editor-only seam for auto-importing models on scene load.
	//
	// A scene references models by their source path (e.g. a raw
	// project://.../Human.gltf). The loader resolves the *baked* mesh sibling; if
	// the model has never been imported, that resolve fails and the entity loses
	// its mesh. In the editor we can bake on demand so a hand-authored or freshly
	// checked-out scene "just works" - this hook carries that bake capability.
	//
	// Only the editor registers it (it links the asset pipeline). The shipped
	// GameRuntime never does: its models come pre-baked in the pak, so scene load
	// must never invoke the pipeline at runtime. When the hook is absent, load
	// behaves exactly as before (warn + skip the mesh).
	struct ModelBakeHook
	{
		// Bakes the model at vfsModelPath if needed. Returns true when the baked
		// mesh is present afterwards (or already was); false + error otherwise.
		std::function<bool(const std::string& vfsModelPath, std::string& error)> ensureBaked;
	};
} // namespace aether::app::scene
