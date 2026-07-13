#pragma once

#include <string>

namespace aether::app
{
	struct EditorProjectContext;
} // namespace aether::app

namespace aether::editor
{
	// Bake a raw glTF/GLB (a VFS path such as "project://assets/models/Fox/Fox.gltf")
	// into the .mesh (+ .skel / .animset / .anim / .material) that the runtime loader
	// needs, writing the outputs next to the source in the project. The editor mounts
	// project:// to the raw project folder, which holds only the .gltf; the baked
	// .mesh normally exists only inside project.pak (built for the shipped runtime),
	// so without this a dragged model can never load in the editor.
	//
	// Returns true (a no-op) if the .mesh already exists. Returns false and fills
	// `error` on failure. Reuses the AssetPipeline MeshProcessor, so the result is
	// byte-identical to what a Publish would pack.
	[[nodiscard]] bool EnsureModelBaked(const std::string& vfsModelPath, const app::EditorProjectContext& project, std::string& error);
} // namespace aether::editor
