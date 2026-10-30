#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "streaming/StreamingBudget.hpp"

namespace aether
{
	class AssetManager;
	class Mesh;
	class AnimationDatabase;
	class SpriteAssetStore;
	class TileAssetStore;

	namespace ui
	{
		class UiRenderer;
	}

	namespace audio
	{
		class AudioSubsystem;
	}
	struct LoadedModel;

	class AssetStreaming
	{
	public:
		explicit AssetStreaming(AssetManager& manager);
		~AssetStreaming();
		AssetStreaming(const AssetStreaming&) = delete;
		AssetStreaming& operator=(const AssetStreaming&) = delete;
		void SetDocumentStores(SpriteAssetStore* sprites, TileAssetStore* tiles, ui::UiRenderer* ui, audio::AudioSubsystem* audio);

		streaming::RequestId Request(std::string_view reference, io::IOPriority priority = io::IOPriority::Normal, bool required = false);
		bool Cancel(streaming::RequestId request);
		bool Release(streaming::RequestId request);
		bool SetRequired(streaming::RequestId request, bool required);
		bool SetPriority(streaming::RequestId request, io::IOPriority priority);
		[[nodiscard]] streaming::State GetRequestState(streaming::RequestId request) const;
		[[nodiscard]] streaming::State GetAssetState(std::string_view reference) const;
		[[nodiscard]] LoadedModel* FindModel(std::string_view reference) const;
		[[nodiscard]] std::shared_ptr<const void> AcquireMeshLease(const Mesh* mesh) const;
		[[nodiscard]] std::shared_ptr<const void> AcquireAnimationLease(const AnimationDatabase* animation) const;
		[[nodiscard]] std::shared_ptr<const void> AcquireDocumentLease(std::string_view reference) const;
		[[nodiscard]] std::shared_ptr<const void> AcquireFontLeaseBySlot(std::uint32_t slot) const;
		Expected<void> SetBudgets(const streaming::Budgets& budgets);
		streaming::BudgetLedger& GetBudget();
		void Update();
		void Shutdown();
		[[nodiscard]] std::string GetDiagnosticsJson() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
} // namespace aether
