#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace aether::net
{
	// A short code a host reads aloud and a joiner types back in, standing in for
	// the room a rendezvous server or a LAN broadcast keys candidates by. It is a
	// lobby code, not a secret - the traversal it sets up is still gated by STUN and
	// a connectivity check - so the only real requirements are that it is easy to
	// say over voice chat and that two hosts starting in the same second land on
	// different ones.
	//
	// Crockford base32: 0-9 and A-Z with I, L, O and U left out, because each is
	// either a near-duplicate of a digit (I/O) or invites a word nobody wants to
	// read off a screen (U completes too many of them). Six characters of it gives
	// over a billion codes, which is plenty to make a same-second collision between
	// two real players vanishingly unlikely.
	inline constexpr std::size_t kRoomCodeLength = 6;

	// A fresh code, drawn from a real entropy source rather than anything seeded
	// from the clock - two players who both press Host in the same second must not
	// be handed the same code.
	[[nodiscard]] std::string NewRoomCode();

	// What a person actually types is not always what NewRoomCode hands out: it may
	// arrive lowercase, with a space or a dash for readability, or with a
	// character the alphabet left out because it looks like one that is in it.
	// This is the one place that forgiveness happens - case is folded, spaces and
	// dashes are dropped, and I/l and O are mapped to the digits they are
	// confusable with - so every wire parser that receives a room code from
	// untrusted input can call this first and trust the result or refuse it
	// outright. Returns nullopt when, even after all that, it is not a room code.
	[[nodiscard]] std::optional<std::string> NormalizeRoomCode(std::string_view text);
} // namespace aether::net
