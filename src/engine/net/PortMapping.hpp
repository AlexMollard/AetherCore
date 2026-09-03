#pragma once

#include <cstdint>
#include <string>

namespace aether::net
{
	// Asks the router to open a port, so a host is reachable without anyone being told to
	// log into their router and forward one by hand.
	//
	// This is the FIRST thing to try, and when it works nothing else is needed: the port
	// is genuinely open, any client can connect straight to it, and no STUN server,
	// signalling channel or punch is involved. Punching (see NatTraversal) is the fallback
	// for the routers that refuse - and it is a fallback, not a replacement, because a
	// symmetric NAT defeats punching entirely but will often still honour a mapping
	// request.
	//
	// Three protocols are spoken, oldest to newest: UPnP-IGD, NAT-PMP and PCP. Which one
	// answers depends on the router and on whether its owner has left the feature enabled,
	// so all are tried and any answer is a good answer.
	//
	// No data socket is touched. This talks only to the gateway, which is exactly why it
	// can sit beside a transport that owns its own socket - unlike a traversal library
	// that insists on binding one.
	class PortMapping
	{
	public:
		enum class State
		{
			Idle,        // nothing requested
			Requesting,  // asking the gateway; no answer yet
			Mapped,      // the port is open, and External* below say where
			Unavailable, // no gateway answered, or it refused
		};

		PortMapping();
		~PortMapping();

		PortMapping(const PortMapping&) = delete;
		PortMapping& operator=(const PortMapping&) = delete;

		// Ask for `internalPort` to be reachable from outside. Returns false only if the
		// library could not start at all; the gateway conversation is asynchronous, so
		// poll GetState() afterwards.
		bool Request(std::uint16_t internalPort);

		// Gives up the mapping. Routers expire them on their own eventually, but a game
		// that closes tidily should not leave a hole open behind it.
		void Release();

		// Drives the state machine. Call once per frame.
		//
		// Deliberately POLLED rather than driven by libplum's completion callback: that
		// callback is invoked from libplum's own internal worker thread, and everything
		// this state feeds - traversal, session setup, UI - lives on the main thread.
		// Polling costs one mutex-guarded struct copy per frame and removes the entire
		// question of marshalling, which is a good trade for a once-per-session event.
		void Tick();

		[[nodiscard]] State GetState() const
		{
			return m_state;
		}

		// The port the outside world must connect to, once Mapped. Often the same as the
		// internal one, but a router with that port already spoken for will hand back a
		// different one, and that is the number a client needs.
		[[nodiscard]] std::uint16_t ExternalPort() const
		{
			return m_externalPort;
		}

		// The address the gateway says this network has, once Mapped. Frequently the
		// public address, which makes a successful mapping a cheaper substitute for a
		// STUN round trip - though behind two layers of NAT it is only the outer router's
		// idea of the world, so it is not a replacement for one.
		[[nodiscard]] const std::string& ExternalHost() const
		{
			return m_externalHost;
		}

		// Why it is Unavailable, for a player who has to be told something.
		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_failure;
		}

	private:
		int m_id = -1; // libplum's mapping handle; -1 when nothing is outstanding
		bool m_libraryHeld = false;
		State m_state = State::Idle;
		std::uint16_t m_internalPort = 0;
		std::uint16_t m_externalPort = 0;
		std::string m_externalHost;
		std::string m_failure;
	};
} // namespace aether::net
