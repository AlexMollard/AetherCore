#include "net/PortMapping.hpp"

#include <cstring>
#include <mutex>

#include <plum/plum.h>

#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// libplum's init is process-wide and not refcounted by the library, exactly like
		// ENet's - see EnetInit.hpp for the reasoning this mirrors. Kept private to this
		// file because nothing else in the engine speaks to a gateway.
		std::mutex g_initMutex;
		int g_initRefCount = 0;

		// Called from libplum's worker thread. The engine logger is mutex-guarded and
		// already written to from the render thread, so this is safe; routing it here
		// rather than letting libplum default to stdout keeps a router's refusal in the
		// same log as everything else that went wrong that session.
		void ForwardLog(plum_log_level_t level, const char* message)
		{
			if (message == nullptr)
			{
				return;
			}
			if (level >= PLUM_LOG_LEVEL_ERROR)
			{
				AE_ERROR(LogCategory::App, "Port mapping: {}", message);
			}
			else
			{
				AE_WARN(LogCategory::App, "Port mapping: {}", message);
			}
		}

		bool AcquireLibrary()
		{
			const std::lock_guard<std::mutex> lock(g_initMutex);
			if (g_initRefCount == 0)
			{
				// Zero-initialised per the header's own instruction: unset fields take
				// documented defaults, and a garbage timeout would be a hang.
				plum_config_t config = {};
				config.log_level = PLUM_LOG_LEVEL_WARN;
				config.log_callback = &ForwardLog;
				if (plum_init(&config) != PLUM_ERR_SUCCESS)
				{
					return false;
				}
			}
			++g_initRefCount;
			return true;
		}

		void ReleaseLibrary()
		{
			const std::lock_guard<std::mutex> lock(g_initMutex);
			if (g_initRefCount > 0 && --g_initRefCount == 0)
			{
				plum_cleanup();
			}
		}
	} // namespace

	PortMapping::PortMapping() = default;

	PortMapping::~PortMapping()
	{
		Release();
	}

	bool PortMapping::Request(std::uint16_t internalPort)
	{
		Release();

		if (!AcquireLibrary())
		{
			m_state = State::Unavailable;
			m_failure = "the port mapping library could not start";
			return false;
		}
		m_libraryHeld = true;

		plum_mapping_t mapping = {};
		// Two fields on two structs are both named `protocol` and take DIFFERENT enums:
		// this one is the IP protocol of the mapping, while plum_config_t::protocol
		// selects which mapping protocol to speak. Getting them the wrong way round
		// compiles cleanly in C and asks for the wrong thing.
		mapping.protocol = PLUM_IP_PROTOCOL_UDP;
		mapping.internal_port = internalPort;
		// A hint, not a demand. Asking for the same number outside as in keeps the
		// address a player reads out loud the same one they see in their own settings;
		// a router that has already given that port away will answer with another.
		mapping.external_port = internalPort;

		// No completion callback: state is polled in Tick, because libplum would deliver
		// this one from its own worker thread. See the note in the header.
		const int id = plum_create_mapping(&mapping, nullptr);
		if (id < 0)
		{
			ReleaseLibrary();
			m_libraryHeld = false;
			m_state = State::Unavailable;
			m_failure = "the port mapping request was refused before it was sent";
			return false;
		}

		m_id = id;
		m_internalPort = internalPort;
		m_state = State::Requesting;
		m_failure.clear();
		return true;
	}

	void PortMapping::Release()
	{
		if (m_id >= 0)
		{
			plum_destroy_mapping(m_id);
			m_id = -1;
		}
		if (m_libraryHeld)
		{
			ReleaseLibrary();
			m_libraryHeld = false;
		}
		m_state = State::Idle;
		m_externalPort = 0;
		m_externalHost.clear();
	}

	void PortMapping::Tick()
	{
		if (m_state != State::Requesting)
		{
			return;
		}

		plum_state_t state = PLUM_STATE_PENDING;
		plum_mapping_t mapping = {};
		if (plum_query_mapping(m_id, &state, &mapping) != PLUM_ERR_SUCCESS)
		{
			m_state = State::Unavailable;
			m_failure = "the port mapping request was lost";
			return;
		}

		switch (state)
		{
			case PLUM_STATE_SUCCESS:
				m_externalPort = mapping.external_port;
				// A fixed inline buffer upstream, but it arrives from the network, so it
				// is read as a bounded string rather than trusted to be terminated.
				m_externalHost.assign(mapping.external_host, strnlen(mapping.external_host, sizeof(mapping.external_host)));
				m_state = State::Mapped;
				AE_INFO(LogCategory::App, "Port mapping: the router opened a port; no forwarding needed.");
				break;

			case PLUM_STATE_FAILURE:
				m_state = State::Unavailable;
				// Named for what a player can do about it. The common cause is a router
				// with UPnP turned off, which is a setting they can change - unlike a
				// carrier-grade NAT, which they cannot.
				m_failure = "no router answered, or it declined to open a port - UPnP or NAT-PMP may be disabled on it";
				break;

			case PLUM_STATE_DESTROYED:
			case PLUM_STATE_DESTROYING:
				m_state = State::Unavailable;
				m_failure = "the port mapping was given up";
				break;

			case PLUM_STATE_PENDING:
				break; // still asking
		}
	}
} // namespace aether::net
