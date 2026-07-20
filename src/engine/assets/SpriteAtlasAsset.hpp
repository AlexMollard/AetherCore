#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "assets/AssetTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	enum class SpriteSliceOrigin : std::uint8_t
	{
		TopLeft = 0,
		BottomLeft,
	};

	struct SpritePixelRect
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		std::int32_t width = 1;
		std::int32_t height = 1;

		auto operator<=>(const SpritePixelRect&) const = default;
	};

	struct SpriteSliceSettings
	{
		std::int32_t cellWidth = 32;
		std::int32_t cellHeight = 32;
		std::int32_t columns = 0;
		std::int32_t rows = 0;
		std::int32_t paddingX = 0;
		std::int32_t paddingY = 0;
		std::int32_t spacingX = 0;
		std::int32_t spacingY = 0;
		SpriteSliceOrigin origin = SpriteSliceOrigin::TopLeft;
		bool trimAlpha = false;
		std::uint8_t alphaThreshold = 1;
	};

	struct SpriteRegion
	{
		AssetObjectId id{};
		std::string persistentKey;
		std::string name;
		SpritePixelRect pixelRect{};
		// Normalised UV begin/end (x0, y0, x1, y1) — the same form the sprite
		// shader lerps between and SpriteRendererComponent.uvRect uses. Derived
		// from pixelRect on load; never persisted.
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
		glm::vec2 pixelSize{1.0f};
		glm::vec2 pivot{0.5f};
		glm::vec4 border{0.0f};
		std::vector<glm::vec2> collisionOutline;
	};

	struct SpriteAtlasReimportDiagnostics
	{
		std::uint32_t preserved = 0;
		std::uint32_t added = 0;
		std::uint32_t removed = 0;
		std::vector<std::string> warnings;

		[[nodiscard]] bool HasReferenceRisk() const noexcept
		{
			return removed != 0;
		}
	};

	struct SpriteSourceImage
	{
		std::int32_t width = 0;
		std::int32_t height = 0;
		std::vector<std::uint8_t> rgbaPixels;
	};

	struct SpriteAtlasAsset
	{
		static constexpr std::uint32_t kSchemaVersion = 2;

		std::uint32_t schemaVersion = kSchemaVersion;
		std::string texturePath;
		std::int32_t textureWidth = 1;
		std::int32_t textureHeight = 1;
		float pixelsPerUnit = 100.0f;
		std::string filterRecommendation = "nearest";
		std::string wrapRecommendation = "clamp";
		std::string importPreset = "grid";
		std::string importProvenance = "native";
		SpriteSliceSettings sliceSettings;
		std::vector<SpriteRegion> sprites;

		[[nodiscard]] static SpriteAtlasAsset WholeTexture(std::string texturePath, glm::vec2 pixelSize, float pixelsPerUnit = 100.0f, std::string_view name = "Sprite");
		[[nodiscard]] static SpriteAtlasAsset SliceGrid(std::string texturePath,
		        std::int32_t textureWidth,
		        std::int32_t textureHeight,
		        const SpriteSliceSettings& settings,
		        const SpriteAtlasAsset* previous = nullptr,
		        std::span<const std::uint8_t> rgbaPixels = {});

		[[nodiscard]] SpriteRegion& AddManualRegion(SpritePixelRect rect, std::string name = "Sprite");
		bool Remove(AssetObjectId id);
		void RecalculateUvs(std::int32_t sourceWidth, std::int32_t sourceHeight);

		[[nodiscard]] const SpriteRegion* Find(AssetObjectId id) const noexcept;
		[[nodiscard]] SpriteRegion* Find(AssetObjectId id) noexcept;
		[[nodiscard]] const SpriteRegion* FindByPersistentKey(std::string_view key) const noexcept;

		[[nodiscard]] Expected<void> Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Expected<SpriteAtlasAsset> Load(const std::filesystem::path& path);
	};

	[[nodiscard]] SpriteAtlasReimportDiagnostics CompareSpriteAtlasReimport(const SpriteAtlasAsset& before, const SpriteAtlasAsset& after);
	[[nodiscard]] Expected<SpriteSourceImage> DecodeSpriteSourceImage(std::string_view path);
} // namespace aether
