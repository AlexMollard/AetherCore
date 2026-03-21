#include "FileRequest.hpp"

namespace meow::io
{
	FileRequest::State FileRequest::GetState() const noexcept
	{
		return m_state.load(std::memory_order_acquire);
	}

	std::span<const std::byte> FileRequest::GetData() const noexcept
	{
		return m_data;
	}

	std::string_view FileRequest::GetError() const noexcept
	{
		return m_error;
	}
}
