#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aether::io
{
	enum class IOPriority
	{
		Critical = 0,
		High = 1,
		Normal = 2,
		Background = 3,
	};

	class FileRequest
	{
	public:
		enum class State
		{
			Pending,
			Complete,
			Failed
		};

		[[nodiscard]] State GetState() const noexcept;
		[[nodiscard]] std::span<const std::byte> GetData() const noexcept;
		[[nodiscard]] std::string_view GetError() const noexcept;

	private:
		friend class IOThread;
		friend class FileSystem;

		std::atomic<State> m_state{State::Pending};
		std::vector<std::byte> m_data;
		std::string m_error;
	};

	using FileRequestHandle = std::shared_ptr<FileRequest>;
} // namespace aether::io
