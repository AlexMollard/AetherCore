#pragma once

#include <optional>
#include <emscripten/fetch.h>

#include "io/MemoryPak.hpp"

namespace aether::web
{
	class WebAssetLoader
	{
	public:
		enum class State
		{
			Idle,
			Loading,
			Ready,
			Failed,
			Stopped
		};
		~WebAssetLoader();
		void Start(const char* url);
		void Stop();

		[[nodiscard]] State GetState() const
		{
			return m_state;
		}

		[[nodiscard]] const std::string& Error() const
		{
			return m_error;
		}

		[[nodiscard]] const io::MemoryPak* Package() const
		{
			return m_package ? &*m_package : nullptr;
		}

	private:
		void Complete(emscripten_fetch_t* fetch, bool success);
		emscripten_fetch_t* m_fetch = nullptr;
		State m_state = State::Idle;
		std::optional<io::MemoryPak> m_package;
		std::string m_error;
	};
} // namespace aether::web
