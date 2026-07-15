#include "editor/AsepriteSpriteImporter.hpp"

#include <algorithm>
#include <fstream>
#include <format>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace aether::editor
{
	namespace
	{
		struct ImportedFrame
		{
			std::string key;
			std::string name;
			SpritePixelRect rect;
			float durationSeconds = 0.1f;
		};

		[[nodiscard]] ImportedFrame ParseFrame(const nlohmann::json& value, std::string fallbackName)
		{
			const nlohmann::json& rect = value.at("frame");
			ImportedFrame frame;
			frame.name = value.value("filename", std::move(fallbackName));
			frame.key = "aseprite:" + frame.name;
			frame.rect = {rect.value("x", 0), rect.value("y", 0), std::max(rect.value("w", 1), 1), std::max(rect.value("h", 1), 1)};
			frame.durationSeconds = std::max(value.value("duration", 100), 1) / 1000.0f;
			return frame;
		}
	} // namespace

	Expected<AsepriteImportResult> ImportAsepriteSpriteMetadata(const std::filesystem::path& jsonPath, std::string texturePath, const SpriteAtlasAsset* previous)
	{
		try
		{
			std::ifstream input(jsonPath);
			if (!input)
			{
				AE_UNEXPECTED(AetherError::Asset("Failed to open Aseprite metadata '" + jsonPath.string() + "'."));
			}
			nlohmann::json root;
			input >> root;
			std::vector<ImportedFrame> importedFrames;
			const nlohmann::json& frames = root.at("frames");
			if (frames.is_array())
			{
				for (std::size_t index = 0; index < frames.size(); ++index)
				{
					importedFrames.push_back(ParseFrame(frames[index], std::format("Frame_{}", index)));
				}
			}
			else
			{
				for (const auto& [name, value]: frames.items())
				{
					importedFrames.push_back(ParseFrame(value, name));
				}
			}

			AsepriteImportResult result;
			result.atlas.texturePath = std::move(texturePath);
			result.atlas.importPreset = "aseprite";
			result.atlas.importProvenance = jsonPath.generic_string();
			if (previous != nullptr)
			{
				result.atlas.pixelsPerUnit = previous->pixelsPerUnit;
				result.atlas.filterRecommendation = previous->filterRecommendation;
				result.atlas.wrapRecommendation = previous->wrapRecommendation;
			}
			const nlohmann::json meta = root.value("meta", nlohmann::json::object());
			const nlohmann::json size = meta.value("size", nlohmann::json::object());
			result.atlas.textureWidth = std::max(size.value("w", 1), 1);
			result.atlas.textureHeight = std::max(size.value("h", 1), 1);
			const AssetId owner = ComputeAssetId(MakeSpriteAtlasSource(result.atlas.texturePath));
			std::unordered_map<std::size_t, AssetObjectId> frameIds;
			for (std::size_t index = 0; index < importedFrames.size(); ++index)
			{
				const ImportedFrame& imported = importedFrames[index];
				const SpriteRegion* old = previous != nullptr ? previous->FindByPersistentKey(imported.key) : nullptr;
				SpriteRegion region;
				region.id = old != nullptr ? old->id : ComputeAssetObjectId(owner, imported.key);
				region.persistentKey = imported.key;
				region.name = old != nullptr ? old->name : imported.name;
				region.pixelRect = imported.rect;
				region.pivot = old != nullptr ? old->pivot : glm::vec2(0.5f);
				region.border = old != nullptr ? old->border : glm::vec4(0.0f);
				region.collisionOutline = old != nullptr ? old->collisionOutline : std::vector<glm::vec2>{};
				frameIds.emplace(index, region.id);
				result.atlas.sprites.push_back(std::move(region));
			}
			result.atlas.RecalculateUvs(result.atlas.textureWidth, result.atlas.textureHeight);

			const nlohmann::json tags = meta.value("frameTags", nlohmann::json::array());
			for (const nlohmann::json& tag: tags)
			{
				SpriteAnimationAsset animation;
				animation.name = tag.value("name", "Animation");
				const std::size_t first = static_cast<std::size_t>(std::max(tag.value("from", 0), 0));
				const std::size_t last = static_cast<std::size_t>(std::max(tag.value("to", static_cast<int>(first)), static_cast<int>(first)));
				const std::string direction = tag.value("direction", "forward");
				animation.loopMode = direction == "pingpong" ? SpriteAnimationLoopMode::PingPong : SpriteAnimationLoopMode::Loop;
				for (std::size_t index = first; index <= last && index < importedFrames.size(); ++index)
				{
					animation.frames.push_back({frameIds[index], importedFrames[index].durationSeconds});
				}
				if (!animation.frames.empty())
				{
					result.animations.push_back(std::move(animation));
				}
			}
			if (result.animations.empty())
			{
				result.diagnostics.push_back("Aseprite metadata contained no frame tags; the atlas was imported without animation clips.");
			}
			return result;
		}
		catch (const std::exception& error)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid Aseprite metadata '" + jsonPath.string() + "': " + error.what()));
		}
	}
} // namespace aether::editor
