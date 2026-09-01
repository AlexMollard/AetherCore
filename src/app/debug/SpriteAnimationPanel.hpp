#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "debug/DebugPanel.hpp"
#include "material/TextureHandle.hpp"

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

		void OnDetach(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;

	public:
		[[nodiscard]] bool HasUnsavedWork(app::LayerContext& context) const override;
		bool SaveUnsavedWork(app::LayerContext& context) override;

	private:
		// Compared structurally rather than flagged at each mutation: frames are added,
		// reordered, retimed and deleted, and a flag that misses one of those reports "saved"
		// over work that is not. Built only when a switch is requested.
		[[nodiscard]] std::uint64_t AnimationSignature() const;
		[[nodiscard]] bool AnimationDirty() const;
		void DrawUnsavedAnimationPrompt(app::LayerContext& context);
		std::uint64_t m_savedAnimationSignature = 0;
		std::string m_pendingAnimationPath;

		void LoadAnimation(app::LayerContext& context, std::string path);
		void LoadAtlas(app::LayerContext& context, std::string path);
		void LoadPreviewTexture(app::LayerContext& context);
		void ReleasePreviewTexture(app::LayerContext& context);
		void SaveAnimation(app::LayerContext& context);
		void DrawSpriteThumbnail(const SpriteRegion* region, ImVec2 size) const;
		void AdvancePreview(float dt);

		// Undo integration: snapshot the clip before an interaction, and on release
		// record a SpriteAnimationEditCommand on the shared history if it changed.
		void CaptureAnimationBaseline();
		void CommitAnimationEdit(app::LayerContext& context);
		void ApplyUndoneAnimation(const SpriteAnimationAsset& animation);

		std::string m_animationPath;
		std::string m_atlasPath;
		SpriteAnimationAsset m_animation;
		SpriteAnimationAsset m_animUndoBaseline;
		bool m_animUndoActive = false;
		SpriteAtlasAsset m_atlas;
		TextureHandle m_previewTexture{};
		std::uint64_t m_previewTextureId = 0;
		std::int32_t m_selectedAtlasRegion = -1;
		std::int32_t m_selectedFrame = -1;
		std::int32_t m_selectedEvent = -1;
		std::uint32_t m_previewFrame = 0;
		std::int32_t m_previewDirection = 1;
		float m_previewFrameTime = 0.0f;
		float m_previewSpeed = 1.0f;
		bool m_previewPlaying = false;
		char m_regionFilter[96]{};
		std::string m_status;
		bool m_statusError = false;
	};
} // namespace aether::editor
