#include "assets/SpriteAtlasAsset.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <sstream>
#include <unordered_set>

#include <stb_image.h>

#include "io/FileSystem.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	namespace
	{
		[[nodiscard]] std::string RegionKey(std::size_t index, std::string_view field)
		{
			return std::format("sprite.{}.{}", index, field);
		}

		[[nodiscard]] std::string PointKey(std::size_t regionIndex, std::size_t pointIndex, std::string_view axis)
		{
			return std::format("sprite.{}.collision.{}.{}", regionIndex, pointIndex, axis);
		}

		[[nodiscard]] std::string IdToString(AssetObjectId id)
		{
			return std::format("{:016x}", id.value);
		}

		[[nodiscard]] AssetObjectId ParseId(std::string_view value)
		{
			AssetObjectId id{};
			const auto result = std::from_chars(value.data(), value.data() + value.size(), id.value, 16);
			return result.ec == std::errc{} ? id : AssetObjectId{};
		}

		[[nodiscard]] SpritePixelRect TrimRect(SpritePixelRect rect, std::int32_t textureWidth, std::int32_t textureHeight, std::span<const std::uint8_t> rgba, std::uint8_t threshold)
		{
			if (rgba.size() < static_cast<std::size_t>(textureWidth) * static_cast<std::size_t>(textureHeight) * 4u)
			{
				return rect;
			}
			std::int32_t minX = rect.x + rect.width;
			std::int32_t minY = rect.y + rect.height;
			std::int32_t maxX = rect.x - 1;
			std::int32_t maxY = rect.y - 1;
			for (std::int32_t y = rect.y; y < rect.y + rect.height; ++y)
			{
				for (std::int32_t x = rect.x; x < rect.x + rect.width; ++x)
				{
					const std::size_t alphaIndex = (static_cast<std::size_t>(y) * static_cast<std::size_t>(textureWidth) + static_cast<std::size_t>(x)) * 4u + 3u;
					if (rgba[alphaIndex] < threshold)
					{
						continue;
					}
					minX = std::min(minX, x);
					minY = std::min(minY, y);
					maxX = std::max(maxX, x);
					maxY = std::max(maxY, y);
				}
			}
			if (maxX < minX || maxY < minY)
			{
				return rect;
			}
			return {minX, minY, maxX - minX + 1, maxY - minY + 1};
		}

		void UpdateRegionGeometry(SpriteRegion& region, std::int32_t textureWidth, std::int32_t textureHeight)
		{
			const float width = static_cast<float>(std::max(textureWidth, 1));
			const float height = static_cast<float>(std::max(textureHeight, 1));
			region.pixelRect.width = std::max(region.pixelRect.width, 1);
			region.pixelRect.height = std::max(region.pixelRect.height, 1);
			region.pixelSize = {static_cast<float>(region.pixelRect.width), static_cast<float>(region.pixelRect.height)};
			// Begin/end form (x0, y0, x1, y1) to match the sprite shader's lerp.
			region.uvRect = {
			        static_cast<float>(region.pixelRect.x) / width,
			        static_cast<float>(region.pixelRect.y) / height,
			        static_cast<float>(region.pixelRect.x + region.pixelRect.width) / width,
			        static_cast<float>(region.pixelRect.y + region.pixelRect.height) / height,
			};
		}
	} // namespace

	SpriteAtlasAsset SpriteAtlasAsset::WholeTexture(std::string sourceTexturePath, glm::vec2 pixelSize, float sourcePixelsPerUnit, std::string_view name)
	{
		SpriteAtlasAsset atlas;
		atlas.texturePath = std::move(sourceTexturePath);
		atlas.pixelsPerUnit = sourcePixelsPerUnit > 0.0f ? sourcePixelsPerUnit : 100.0f;
		atlas.importPreset = "whole-texture";
		const AssetId owner = ComputeAssetId(MakeSpriteAtlasSource(atlas.texturePath));
		const glm::vec2 safeSize = glm::max(pixelSize, glm::vec2(1.0f));
		atlas.textureWidth = static_cast<std::int32_t>(safeSize.x);
		atlas.textureHeight = static_cast<std::int32_t>(safeSize.y);
		atlas.sprites.push_back(SpriteRegion{
		        .id = ComputeAssetObjectId(owner, "whole-texture"),
		        .persistentKey = "whole-texture",
		        .name = std::string(name),
		        .pixelRect = {0, 0, static_cast<std::int32_t>(safeSize.x), static_cast<std::int32_t>(safeSize.y)},
		        .uvRect = {0.0f, 0.0f, 1.0f, 1.0f},
		        .pixelSize = safeSize,
		        .pivot = {0.5f, 0.5f},
		});
		return atlas;
	}

	SpriteAtlasAsset SpriteAtlasAsset::SliceGrid(std::string sourceTexturePath,
	        std::int32_t textureWidth,
	        std::int32_t textureHeight,
	        const SpriteSliceSettings& settings,
	        const SpriteAtlasAsset* previous,
	        std::span<const std::uint8_t> rgbaPixels)
	{
		SpriteAtlasAsset atlas;
		atlas.texturePath = std::move(sourceTexturePath);
		atlas.textureWidth = std::max(textureWidth, 1);
		atlas.textureHeight = std::max(textureHeight, 1);
		atlas.sliceSettings = settings;
		if (previous != nullptr)
		{
			atlas.pixelsPerUnit = previous->pixelsPerUnit;
			atlas.filterRecommendation = previous->filterRecommendation;
			atlas.wrapRecommendation = previous->wrapRecommendation;
		}

		const std::int32_t cellWidth = std::max(settings.cellWidth, 1);
		const std::int32_t cellHeight = std::max(settings.cellHeight, 1);
		const std::int32_t availableWidth = std::max(textureWidth - settings.paddingX * 2 + settings.spacingX, 0);
		const std::int32_t availableHeight = std::max(textureHeight - settings.paddingY * 2 + settings.spacingY, 0);
		const std::int32_t columns = settings.columns > 0 ? settings.columns : availableWidth / std::max(cellWidth + settings.spacingX, 1);
		const std::int32_t rows = settings.rows > 0 ? settings.rows : availableHeight / std::max(cellHeight + settings.spacingY, 1);
		const AssetId owner = ComputeAssetId(MakeSpriteAtlasSource(atlas.texturePath));
		for (std::int32_t row = 0; row < rows; ++row)
		{
			for (std::int32_t column = 0; column < columns; ++column)
			{
				const std::int32_t sourceRow = settings.origin == SpriteSliceOrigin::BottomLeft ? rows - row - 1 : row;
				SpritePixelRect rect{
				        settings.paddingX + column * (cellWidth + settings.spacingX),
				        settings.paddingY + sourceRow * (cellHeight + settings.spacingY),
				        cellWidth,
				        cellHeight,
				};
				if (rect.x < 0 || rect.y < 0 || rect.x + rect.width > textureWidth || rect.y + rect.height > textureHeight)
				{
					continue;
				}
				if (settings.trimAlpha)
				{
					rect = TrimRect(rect, textureWidth, textureHeight, rgbaPixels, settings.alphaThreshold);
				}
				const std::string persistentKey = std::format("grid:{}:{}", row, column);
				const SpriteRegion* previousRegion = previous != nullptr ? previous->FindByPersistentKey(persistentKey) : nullptr;
				SpriteRegion region;
				region.id = previousRegion != nullptr ? previousRegion->id : ComputeAssetObjectId(owner, persistentKey);
				region.persistentKey = persistentKey;
				region.name = previousRegion != nullptr ? previousRegion->name : std::format("Sprite_{}_{}", row, column);
				region.pixelRect = rect;
				region.pivot = previousRegion != nullptr ? previousRegion->pivot : glm::vec2(0.5f);
				region.border = previousRegion != nullptr ? previousRegion->border : glm::vec4(0.0f);
				region.collisionOutline = previousRegion != nullptr ? previousRegion->collisionOutline : std::vector<glm::vec2>{};
				UpdateRegionGeometry(region, textureWidth, textureHeight);
				atlas.sprites.push_back(std::move(region));
			}
		}
		return atlas;
	}

	SpriteRegion& SpriteAtlasAsset::AddManualRegion(SpritePixelRect rect, std::string name)
	{
		std::uint32_t suffix = 1;
		std::string key;
		do
		{
			key = std::format("manual:{}", suffix++);
		} while (FindByPersistentKey(key) != nullptr);
		const AssetId owner = ComputeAssetId(MakeSpriteAtlasSource(texturePath));
		sprites.push_back(SpriteRegion{
		        .id = ComputeAssetObjectId(owner, key),
		        .persistentKey = std::move(key),
		        .name = std::move(name),
		        .pixelRect = rect,
		        .pixelSize = {static_cast<float>(std::max(rect.width, 1)), static_cast<float>(std::max(rect.height, 1))},
		});
		return sprites.back();
	}

	bool SpriteAtlasAsset::Remove(AssetObjectId id)
	{
		const auto it = std::ranges::find(sprites, id, &SpriteRegion::id);
		if (it == sprites.end())
		{
			return false;
		}
		sprites.erase(it);
		return true;
	}

	void SpriteAtlasAsset::RecalculateUvs(std::int32_t sourceWidth, std::int32_t sourceHeight)
	{
		for (SpriteRegion& sprite: sprites)
		{
			UpdateRegionGeometry(sprite, sourceWidth, sourceHeight);
		}
	}

	const SpriteRegion* SpriteAtlasAsset::Find(AssetObjectId id) const noexcept
	{
		const auto it = std::ranges::find(sprites, id, &SpriteRegion::id);
		return it != sprites.end() ? &*it : nullptr;
	}

	SpriteRegion* SpriteAtlasAsset::Find(AssetObjectId id) noexcept
	{
		const auto it = std::ranges::find(sprites, id, &SpriteRegion::id);
		return it != sprites.end() ? &*it : nullptr;
	}

	const SpriteRegion* SpriteAtlasAsset::FindByPersistentKey(std::string_view key) const noexcept
	{
		const auto it = std::ranges::find(sprites, key, &SpriteRegion::persistentKey);
		return it != sprites.end() ? &*it : nullptr;
	}

	Expected<void> SpriteAtlasAsset::Save(const std::filesystem::path& path) const
	{
		TomlConfig config;
		config.Set("atlas.schema_version", static_cast<float>(kSchemaVersion));
		config.Set("atlas.texture", texturePath);
		config.Set("atlas.texture_width", static_cast<float>(textureWidth));
		config.Set("atlas.texture_height", static_cast<float>(textureHeight));
		config.Set("atlas.pixels_per_unit", pixelsPerUnit);
		config.Set("atlas.filter", filterRecommendation);
		config.Set("atlas.wrap", wrapRecommendation);
		config.Set("atlas.import_preset", importPreset);
		config.Set("atlas.import_provenance", importProvenance);
		config.Set("slice.cell_width", static_cast<float>(sliceSettings.cellWidth));
		config.Set("slice.cell_height", static_cast<float>(sliceSettings.cellHeight));
		config.Set("slice.columns", static_cast<float>(sliceSettings.columns));
		config.Set("slice.rows", static_cast<float>(sliceSettings.rows));
		config.Set("slice.padding_x", static_cast<float>(sliceSettings.paddingX));
		config.Set("slice.padding_y", static_cast<float>(sliceSettings.paddingY));
		config.Set("slice.spacing_x", static_cast<float>(sliceSettings.spacingX));
		config.Set("slice.spacing_y", static_cast<float>(sliceSettings.spacingY));
		config.Set("slice.origin", std::string_view{sliceSettings.origin == SpriteSliceOrigin::BottomLeft ? "bottom-left" : "top-left"});
		config.Set("slice.trim_alpha", sliceSettings.trimAlpha);
		config.Set("slice.alpha_threshold", static_cast<float>(sliceSettings.alphaThreshold));
		config.Set("atlas.sprite_count", static_cast<float>(sprites.size()));
		for (std::size_t index = 0; index < sprites.size(); ++index)
		{
			const SpriteRegion& sprite = sprites[index];
			config.Set(RegionKey(index, "id"), IdToString(sprite.id));
			config.Set(RegionKey(index, "persistent_key"), sprite.persistentKey);
			config.Set(RegionKey(index, "name"), sprite.name);
			config.Set(RegionKey(index, "x"), static_cast<float>(sprite.pixelRect.x));
			config.Set(RegionKey(index, "y"), static_cast<float>(sprite.pixelRect.y));
			config.Set(RegionKey(index, "width"), static_cast<float>(sprite.pixelRect.width));
			config.Set(RegionKey(index, "height"), static_cast<float>(sprite.pixelRect.height));
			config.Set(RegionKey(index, "pivot_x"), sprite.pivot.x);
			config.Set(RegionKey(index, "pivot_y"), sprite.pivot.y);
			config.Set(RegionKey(index, "border_left"), sprite.border.x);
			config.Set(RegionKey(index, "border_top"), sprite.border.y);
			config.Set(RegionKey(index, "border_right"), sprite.border.z);
			config.Set(RegionKey(index, "border_bottom"), sprite.border.w);
			config.Set(RegionKey(index, "collision_count"), static_cast<float>(sprite.collisionOutline.size()));
			for (std::size_t pointIndex = 0; pointIndex < sprite.collisionOutline.size(); ++pointIndex)
			{
				config.Set(PointKey(index, pointIndex, "x"), sprite.collisionOutline[pointIndex].x);
				config.Set(PointKey(index, pointIndex, "y"), sprite.collisionOutline[pointIndex].y);
			}
		}
		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			std::ostringstream stream;
			config.Save(stream, "AetherCore sprite atlas");
			AE_TRY_VOID(io::FileSystem::WriteFileText(pathString, stream.str()));
		}
		else if (!config.SaveToPath(path, "AetherCore sprite atlas"))
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to save sprite atlas '" + path.string() + "'."));
		}
		return {};
	}

	Expected<SpriteAtlasAsset> SpriteAtlasAsset::Load(const std::filesystem::path& path)
	{
		TomlConfig config;
		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			AE_TRY(text, io::FileSystem::ReadFileText(pathString));
			if (!config.Load(*text))
			{
				AE_UNEXPECTED(AetherError::Asset("Failed to parse sprite atlas '" + path.string() + "'."));
			}
		}
		else if (!config.LoadFromPath(path))
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to load sprite atlas '" + path.string() + "'."));
		}
		SpriteAtlasAsset atlas;
		atlas.schemaVersion = static_cast<std::uint32_t>(config.GetFloat("atlas.schema_version", 1.0f));
		atlas.texturePath = config.GetString("atlas.texture");
		atlas.textureWidth = std::max(static_cast<std::int32_t>(config.GetFloat("atlas.texture_width", 1.0f)), 1);
		atlas.textureHeight = std::max(static_cast<std::int32_t>(config.GetFloat("atlas.texture_height", 1.0f)), 1);
		atlas.pixelsPerUnit = std::max(config.GetFloat("atlas.pixels_per_unit", 100.0f), 0.001f);
		atlas.filterRecommendation = config.GetString("atlas.filter", "nearest");
		atlas.wrapRecommendation = config.GetString("atlas.wrap", "clamp");
		atlas.importPreset = config.GetString("atlas.import_preset", "grid");
		atlas.importProvenance = config.GetString("atlas.import_provenance", "native");
		atlas.sliceSettings.cellWidth = static_cast<std::int32_t>(config.GetFloat("slice.cell_width", 32.0f));
		atlas.sliceSettings.cellHeight = static_cast<std::int32_t>(config.GetFloat("slice.cell_height", 32.0f));
		atlas.sliceSettings.columns = static_cast<std::int32_t>(config.GetFloat("slice.columns", 0.0f));
		atlas.sliceSettings.rows = static_cast<std::int32_t>(config.GetFloat("slice.rows", 0.0f));
		atlas.sliceSettings.paddingX = static_cast<std::int32_t>(config.GetFloat("slice.padding_x", 0.0f));
		atlas.sliceSettings.paddingY = static_cast<std::int32_t>(config.GetFloat("slice.padding_y", 0.0f));
		atlas.sliceSettings.spacingX = static_cast<std::int32_t>(config.GetFloat("slice.spacing_x", 0.0f));
		atlas.sliceSettings.spacingY = static_cast<std::int32_t>(config.GetFloat("slice.spacing_y", 0.0f));
		atlas.sliceSettings.origin = config.GetString("slice.origin", "top-left") == "bottom-left" ? SpriteSliceOrigin::BottomLeft : SpriteSliceOrigin::TopLeft;
		atlas.sliceSettings.trimAlpha = config.GetBool("slice.trim_alpha", false);
		atlas.sliceSettings.alphaThreshold = static_cast<std::uint8_t>(std::clamp(config.GetFloat("slice.alpha_threshold", 1.0f), 0.0f, 255.0f));
		const std::size_t spriteCount = static_cast<std::size_t>(std::max(config.GetFloat("atlas.sprite_count", 0.0f), 0.0f));
		atlas.sprites.reserve(spriteCount);
		for (std::size_t index = 0; index < spriteCount; ++index)
		{
			SpriteRegion sprite;
			sprite.id = ParseId(config.GetString(RegionKey(index, "id")));
			sprite.persistentKey = config.GetString(RegionKey(index, "persistent_key"), std::format("legacy:{}", index));
			sprite.name = config.GetString(RegionKey(index, "name"), std::format("Sprite_{}", index));
			sprite.pixelRect = {
			        static_cast<std::int32_t>(config.GetFloat(RegionKey(index, "x"), 0.0f)),
			        static_cast<std::int32_t>(config.GetFloat(RegionKey(index, "y"), 0.0f)),
			        std::max(static_cast<std::int32_t>(config.GetFloat(RegionKey(index, "width"), 1.0f)), 1),
			        std::max(static_cast<std::int32_t>(config.GetFloat(RegionKey(index, "height"), 1.0f)), 1),
			};
			sprite.pixelSize = {static_cast<float>(sprite.pixelRect.width), static_cast<float>(sprite.pixelRect.height)};
			sprite.pivot = {
			        config.GetFloat(RegionKey(index, "pivot_x"), 0.5f),
			        config.GetFloat(RegionKey(index, "pivot_y"), 0.5f),
			};
			sprite.border = {
			        config.GetFloat(RegionKey(index, "border_left"), 0.0f),
			        config.GetFloat(RegionKey(index, "border_top"), 0.0f),
			        config.GetFloat(RegionKey(index, "border_right"), 0.0f),
			        config.GetFloat(RegionKey(index, "border_bottom"), 0.0f),
			};
			const std::size_t collisionCount = static_cast<std::size_t>(std::max(config.GetFloat(RegionKey(index, "collision_count"), 0.0f), 0.0f));
			sprite.collisionOutline.reserve(collisionCount);
			for (std::size_t pointIndex = 0; pointIndex < collisionCount; ++pointIndex)
			{
				sprite.collisionOutline.emplace_back(config.GetFloat(PointKey(index, pointIndex, "x"), 0.0f), config.GetFloat(PointKey(index, pointIndex, "y"), 0.0f));
			}
			UpdateRegionGeometry(sprite, atlas.textureWidth, atlas.textureHeight);
			atlas.sprites.push_back(std::move(sprite));
		}
		return atlas;
	}

	SpriteAtlasReimportDiagnostics CompareSpriteAtlasReimport(const SpriteAtlasAsset& before, const SpriteAtlasAsset& after)
	{
		SpriteAtlasReimportDiagnostics diagnostics;
		std::unordered_set<std::uint64_t> beforeIds;
		std::unordered_set<std::uint64_t> afterIds;
		for (const SpriteRegion& sprite: before.sprites)
		{
			beforeIds.insert(sprite.id.value);
		}
		for (const SpriteRegion& sprite: after.sprites)
		{
			afterIds.insert(sprite.id.value);
			if (beforeIds.contains(sprite.id.value))
			{
				++diagnostics.preserved;
			}
			else
			{
				++diagnostics.added;
			}
		}
		for (const SpriteRegion& sprite: before.sprites)
		{
			if (!afterIds.contains(sprite.id.value))
			{
				++diagnostics.removed;
				diagnostics.warnings.push_back(std::format("Sprite '{}' ({}) no longer exists after reimport.", sprite.name, IdToString(sprite.id)));
			}
		}
		return diagnostics;
	}

	Expected<SpriteSourceImage> DecodeSpriteSourceImage(std::string_view path)
	{
		AE_TRY(fileData, io::FileSystem::ReadFile(path));
		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData->data()), static_cast<int>(fileData->size()), &width, &height, &channels, STBI_rgb_alpha);
		if (pixels == nullptr)
		{
			AE_UNEXPECTED(AetherError::Asset("Failed to decode sprite source '" + std::string(path) + "': " + stbi_failure_reason()));
		}
		SpriteSourceImage image;
		image.width = width;
		image.height = height;
		image.rgbaPixels.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
		stbi_image_free(pixels);
		return image;
	}
} // namespace aether
