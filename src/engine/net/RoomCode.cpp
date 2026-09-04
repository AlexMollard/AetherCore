#include "net/RoomCode.hpp"

#include <cctype>
#include <random>

namespace aether::net
{
	namespace
	{
		// The Crockford alphabet, in the order a symbol value indexes into it. I, L,
		// O and U are simply absent - there is no separate "excluded" list to check
		// against, because a character that is not found here already is not one.
		constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

		// Folds one character to the alphabet member it means, if any. Case is
		// normalized first so the confusable checks only need to name one case.
		// I/L and O are mapped because they are visually confusable with the digits
		// 1 and 0 respectively - the reason the alphabet leaves them out in the
		// first place. U gets no such mapping: it is not confusable with anything
		// already in the alphabet, so a U in the input is simply not a room code.
		std::optional<char> FoldChar(char c)
		{
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			if (c == 'I' || c == 'L')
			{
				return '1';
			}
			if (c == 'O')
			{
				return '0';
			}
			if (kAlphabet.find(c) == std::string_view::npos)
			{
				return std::nullopt;
			}
			return c;
		}
	} // namespace

	std::string NewRoomCode()
	{
		// Seeded once per thread from std::random_device rather than reseeded on
		// every call: a lobby code is generated freely - every Host press wants one
		// - and paying random_device's cost (a real syscall on most platforms, and a
		// blocking one on some) per call would make Host feel like it hitches for no
		// reason a player could ever see.
		thread_local std::mt19937_64 engine = [] {
			std::random_device seedSource;
			const std::uint64_t seed = (static_cast<std::uint64_t>(seedSource()) << 32) | seedSource();
			return std::mt19937_64{seed};
		}();

		std::uniform_int_distribution<std::size_t> pick(0, kAlphabet.size() - 1);
		std::string code(kRoomCodeLength, '0');
		for (char& c: code)
		{
			c = kAlphabet[pick(engine)];
		}
		return code;
	}

	std::optional<std::string> NormalizeRoomCode(std::string_view text)
	{
		std::string out;
		out.reserve(kRoomCodeLength);
		for (const char c: text)
		{
			if (c == ' ' || c == '-')
			{
				continue; // read out with a break for pronounceability, dropped here
			}
			const auto folded = FoldChar(c);
			if (!folded.has_value())
			{
				return std::nullopt; // not a character this alphabet can mean
			}
			if (out.size() >= kRoomCodeLength)
			{
				return std::nullopt; // longer than a real code once separators are gone
			}
			out.push_back(*folded);
		}
		if (out.size() != kRoomCodeLength)
		{
			return std::nullopt; // shorter than a real code
		}
		return out;
	}
} // namespace aether::net
