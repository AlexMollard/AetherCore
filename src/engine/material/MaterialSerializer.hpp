#pragma once

#include <string>
#include <string_view>

#include "material/MaterialAsset.hpp"

namespace aether
{
	class TextureRegistry;

	// A material as it exists on DISK, before its textures have been acquired.
	//
	// MaterialAsset holds TextureHandles, which are registry indices and mean nothing in a
	// file. This is the same material with those slots still as paths, which is what both
	// reading and writing need.
	struct MaterialPresetSpec
	{
		MaterialAsset material{};
		std::string albedoPath;
		std::string normalPath;
		std::string metallicRoughnessPath;
		std::string occlusionPath;
		std::string emissivePath;
		std::string shaderVfsPath;
		// The node graph that generated this material's shader, as raw TOML, or empty when
		// the material was authored by hand.
		//
		// Carried as TEXT because the graph belongs to the editor and the engine has no
		// business knowing what a node is: it round-trips the block untouched, so a graph can
		// grow new node types without the engine changing at all. It lives in the material
		// file rather than beside it so that a material is ONE file - a sidecar meant every
		// material had a second file to keep in step, rename with it, and remember to delete.
		std::string graphSection;
	};

	// Read and write the authored TOML form of a material.
	//
	// Both directions live together on purpose: the writer must emit exactly the keys the
	// reader accepts, and when they lived apart the reader silently had no key for
	// modulateVertexColor or receiveShadows - a field the inspector could edit but a file
	// could never carry. Keeping them adjacent is what makes a round-trip testable, and
	// MaterialSerializerTests does exactly that.
	//
	// The binary `.material` the model importer emits is the COOKED form of the same data.
	// This text form is the one a person edits and git diffs.
	namespace MaterialSerializer
	{
		// `path` is the material's own VFS path, used to resolve relative texture references.
		[[nodiscard]] MaterialPresetSpec Parse(std::string_view path, const std::string& text);

		[[nodiscard]] std::string ToToml(const MaterialPresetSpec& spec);

		// Resolves each texture handle back to the path it was acquired from, so a material
		// edited in memory can be written back out.
		[[nodiscard]] MaterialPresetSpec Describe(const MaterialAsset& material, const TextureRegistry& textures);
	} // namespace MaterialSerializer
} // namespace aether
