#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class SpriteAnimationPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Sprite Animation";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void LoadAtlas(app::LayerContext& context);
		void AdvancePreview(float dt);

		std::array<char, 512> m_animationPath{};
		std::array<char, 512> m_atlasPath{};
		SpriteAnimationAsset m_animation;
		SpriteAtlasAsset m_atlas;
		std::int32_t m_selectedAtlasRegion = -1;
		std::int32_t m_selectedFrame = -1;
		std::int32_t m_selectedEvent = -1;
		std::uint32_t m_previewFrame = 0;
		std::int32_t m_previewDirection = 1;
		float m_previewFrameTime = 0.0f;
		float m_previewSpeed = 1.0f;
		bool m_previewPlaying = false;
		std::string m_status;
		bool m_statusError = false;
	};
} // namespace aether::editor
