#include "WebAssetLoader.hpp"

#include <cstring>
#include "WebAssetFormat.hpp"

namespace aether::web
{
	namespace
	{
		void CloseFetch(emscripten_fetch_t* fetch)
		{
			// close() may synchronously invoke onerror while aborting a request.
			// Detach callback ownership before freeing it to prevent recursive close.
			fetch->userData = nullptr;
			emscripten_fetch_close(fetch);
		}
	} // namespace

	WebAssetLoader::~WebAssetLoader()
	{
		Stop();
	}

	void WebAssetLoader::Start(const char* url)
	{
		if (m_state != State::Idle)
		{
			return;
		}
		m_state = State::Loading;
		emscripten_fetch_attr_t attributes;
		emscripten_fetch_attr_init(&attributes);
		std::strcpy(attributes.requestMethod, "GET");
		attributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
		attributes.timeoutMSecs = 30000;
		attributes.userData = this;
		attributes.onsuccess = [](emscripten_fetch_t* fetch)
		{
			if (auto* loader = static_cast<WebAssetLoader*>(fetch->userData))
			{
				loader->Complete(fetch, true);
			}
		};
		attributes.onerror = [](emscripten_fetch_t* fetch)
		{
			if (auto* loader = static_cast<WebAssetLoader*>(fetch->userData))
			{
				loader->Complete(fetch, false);
			}
		};
		attributes.onprogress = [](emscripten_fetch_t* fetch)
		{
			if (fetch->userData && (fetch->totalBytes > io::MemoryPak::kMaxPackageBytes || fetch->dataOffset > io::MemoryPak::kMaxPackageBytes))
			{
				auto& loader = *static_cast<WebAssetLoader*>(fetch->userData);
				loader.m_state = State::Failed;
				loader.m_error = "Package exceeds the 256 MiB browser limit";
				loader.m_fetch = nullptr;

				CloseFetch(fetch);
			}
		};
		m_fetch = emscripten_fetch(&attributes, url);
		if (!m_fetch)
		{
			m_error = "Could not start asynchronous package download";
			m_state = State::Failed;
		}
	}

	void WebAssetLoader::Complete(emscripten_fetch_t* fetch, bool success)
	{
		m_fetch = nullptr;
		if (m_state == State::Loading)
		{
			if (!success || fetch->status != 200)
			{
				m_error = "Package download failed (HTTP " + std::to_string(fetch->status) + ")";
				m_state = State::Failed;
			}
			else
			{
				auto package = io::MemoryPak::Open({reinterpret_cast<const std::byte*>(fetch->data), static_cast<size_t>(fetch->numBytes)}, AE_WEB_ASSET_TARGET);
				if (package)
				{
					m_package = std::move(*package);
					m_state = State::Ready;
				}
				else
				{
					m_error = package.error().message;
					m_state = State::Failed;
				}
			}
		}

		CloseFetch(fetch);
	}

	void WebAssetLoader::Stop()
	{
		if (m_fetch)
		{
			auto* fetch = m_fetch;
			m_fetch = nullptr;
			m_state = State::Stopped;

			CloseFetch(fetch);
		}
		m_package.reset();
		if (m_state != State::Failed)
		{
			m_state = State::Stopped;
		}
	}
} // namespace aether::web
