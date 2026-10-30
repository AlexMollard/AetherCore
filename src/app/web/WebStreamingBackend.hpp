#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "scene/WorldStreaming.hpp"
#include "web/WebScene.hpp"
#include "streaming/StreamingBudget.hpp"

namespace aether::app::web
{
	struct WebStreamingCallbacks
	{
		std::function<void(std::shared_ptr<streaming::BudgetLedger>)> bindBudget;
		std::function<Expected<std::optional<WebRegion>>(std::string_view, std::string_view, std::span<const std::byte>, std::uint64_t)> attach;
		std::function<Expected<bool>(WebRegion&)> start;
		std::function<void(WebRegion&)> commit;
		std::function<void(std::string_view)> cancelAttach;
		std::function<Expected<void>(WebRegion&)> detach;
		std::function<bool()> ready;
		std::function<bool()> budgetBlocked;
		std::function<Expected<std::string>(const WebRegion&)> capture;
		std::function<Expected<void>(WebRegion&, std::string_view)> restore;
		std::function<Expected<bool>(WebRegion&, std::string_view)> restoreScripts;
		std::function<bool(const std::vector<Entity>&)> promotedAlive;
		std::function<std::vector<Entity>(std::string_view)> persistentRoots;
		std::function<Expected<std::string>(Entity)> persistentAssets;
		std::function<Expected<std::uint64_t>(Entity)> measurePersistentAssets;
		std::function<bool()> retirementPending;
	};

	Expected<std::unique_ptr<scene::IWorldStreamingBackend>> CreateWebStreamingBackend(std::string_view manifestUrl, WebStreamingCallbacks callbacks);
} // namespace aether::app::web
