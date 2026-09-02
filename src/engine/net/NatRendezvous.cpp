#include "net/NatRendezvous.hpp"

#include <algorithm>

namespace aether::net
{
	namespace
	{
		// How long to wait for the peer to say anything at all. Distinct from the punch
		// timeout underneath: a peer that never publishes and a peer that publishes but
		// cannot be reached are different failures, and telling someone to check their
		// NAT when their friend simply has not pressed Join yet is a wasted evening.
		constexpr float kPeerTimeout = 20.0f;
	} // namespace

	NatRendezvous::NatRendezvous(NatTraversal& traversal, ISignalingChannel& channel, std::uint16_t localPort)
	      : m_traversal(traversal),
	        m_channel(channel),
	        m_localPort(localPort)
	{
	}

	void NatRendezvous::PublishKnownCandidates()
	{
		CandidateSet set;
		set.endpoints = NatTraversal::LocalCandidates(m_localPort);
		if (const auto reflexive = m_traversal.PublicEndpoint())
		{
			set.endpoints.push_back(*reflexive);
		}
		if (!set.endpoints.empty())
		{
			m_channel.Publish(set);
		}
	}

	void NatRendezvous::Begin(const std::string& stunHost, std::uint16_t stunPort)
	{
		m_begun = true;
		m_elapsed = 0.0f;
		m_timedOut = false;
		m_failure.clear();
		m_peerCandidates.clear();
		m_publishedPublic = false;
		m_punchStarted = false;

		// Said before asking. A peer on the same network can act on this straight away,
		// and one behind a NAT loses nothing by hearing the LAN addresses first.
		PublishKnownCandidates();

		if (stunHost.empty())
		{
			// No NAT to ask about. Local candidates are the whole offer, and the peer's
			// answer is all that is still wanted.
			m_publishedPublic = true;
			return;
		}
		m_traversal.BeginDiscovery(stunHost, stunPort);
	}

	void NatRendezvous::Tick(float deltaSeconds)
	{
		if (!m_begun || m_timedOut)
		{
			return;
		}

		const State state = m_traversal.GetState();
		if (state == State::Open)
		{
			return;
		}
		// A failure before any punch was attempted is discovery giving up, not the
		// traversal doing so: an unreachable STUN server costs the public candidate and
		// nothing else, and the peer's address on our own network is still worth trying.
		// Only a failed punch is terminal.
		if (state == State::Failed && m_punchStarted)
		{
			return;
		}

		m_elapsed += deltaSeconds;
		m_traversal.Tick(deltaSeconds);

		// Discovery answering is news the peer needs: until it has our public endpoint it
		// has nothing outside our LAN to punch at.
		if (!m_publishedPublic && m_traversal.PublicEndpoint().has_value())
		{
			m_publishedPublic = true;
			PublishKnownCandidates();
		}

		if (const auto offered = m_channel.Poll())
		{
			bool grew = false;
			for (const Endpoint& candidate: offered->endpoints)
			{
				if (std::ranges::find(m_peerCandidates, candidate) == m_peerCandidates.end())
				{
					m_peerCandidates.push_back(candidate);
					grew = true;
				}
			}
			// Restarted only when something genuinely new arrived. A candidate that has
			// not been tried deserves the full punch window, but a peer republishing the
			// same list must not be able to hold the attempt open forever.
			if (grew)
			{
				m_punchStarted = true;
				m_traversal.BeginPunch(m_peerCandidates);
			}
		}

		if (m_peerCandidates.empty() && m_elapsed >= kPeerTimeout)
		{
			m_timedOut = true;
			m_failure = "the peer never offered an address - it may not have joined yet";
		}
	}

	NatRendezvous::State NatRendezvous::GetState() const
	{
		if (m_timedOut)
		{
			return State::Failed;
		}
		const State state = m_traversal.GetState();
		// Discovery having given up is not the rendezvous having given up - see Tick. It
		// must not be reported as one, or a caller watching for Failed abandons an attempt
		// that is still perfectly capable of succeeding over the LAN.
		if (state == State::Failed && !m_punchStarted)
		{
			return State::Discovering;
		}
		return state;
	}

	std::optional<NatRendezvous::Endpoint> NatRendezvous::OpenPath() const
	{
		return m_traversal.OpenPath();
	}

	const std::string& NatRendezvous::FailureReason() const
	{
		return m_timedOut ? m_failure : m_traversal.FailureReason();
	}
} // namespace aether::net
