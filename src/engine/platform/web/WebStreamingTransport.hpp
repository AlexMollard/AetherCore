#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include "streaming/StreamingBudget.hpp"

namespace aether::web
{
	class WebStreamingTransport
	{
	public:
		using RequestId = streaming::RequestId;
		using State = streaming::State;
		WebStreamingTransport();
		~WebStreamingTransport();
		Expected<void> Initialize(std::string_view manifestUrl, streaming::BudgetLedger& budget);
		RequestId Acquire(std::string_view sceneReference, io::IOPriority priority);
		RequestId PrepareSnapshot(std::string_view sceneToml, io::IOPriority priority = io::IOPriority::Normal);
		// Borrowed until the matching snapshot request is released or cancelled.
		std::span<const std::byte> GetPreparedSnapshot(RequestId id) const;
		bool Cancel(RequestId id);
		bool Release(RequestId id);
		bool SetPriority(RequestId id, io::IOPriority priority);
		State GetState(RequestId id) const;
		std::string GetError(RequestId id) const;
		void Update();
		void Shutdown();
		std::string GetDiagnosticsJson() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
} // namespace aether::web
