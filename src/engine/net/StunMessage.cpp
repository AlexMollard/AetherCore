#include "net/StunMessage.hpp"

#include <chrono>
#include <random>

namespace aether::net::stun
{
	namespace
	{
		constexpr std::uint16_t kBindingRequest = 0x0001;
		constexpr std::uint16_t kBindingSuccess = 0x0101;
		constexpr std::uint16_t kAttrMappedAddress = 0x0001;
		constexpr std::uint16_t kAttrXorMappedAddress = 0x0020;
		constexpr std::uint8_t kFamilyIpv4 = 0x01;

		// Everything on the wire is big-endian, and saying so at each read beats one
		// htonl at the boundary that every later reader has to remember is there.
		std::uint16_t ReadU16(const std::uint8_t* p)
		{
			return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
		}

		std::uint32_t ReadU32(const std::uint8_t* p)
		{
			return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
		}

		void WriteU16(std::uint8_t* p, std::uint16_t v)
		{
			p[0] = static_cast<std::uint8_t>(v >> 8);
			p[1] = static_cast<std::uint8_t>(v & 0xFF);
		}

		void WriteU32(std::uint8_t* p, std::uint32_t v)
		{
			p[0] = static_cast<std::uint8_t>(v >> 24);
			p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
			p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
			p[3] = static_cast<std::uint8_t>(v & 0xFF);
		}

		const std::uint8_t* Bytes(std::span<const std::byte> s)
		{
			return reinterpret_cast<const std::uint8_t*>(s.data());
		}
	} // namespace

	TransactionId MakeTransactionId()
	{
		// Seeded per call from the clock as well as random_device: on the platforms where
		// random_device is a fixed sequence (some MinGW builds), seeding from it alone
		// would hand every peer on every run the same "random" id.
		static thread_local std::mt19937 engine{[]
		        {
			        std::random_device device;
			        const auto now = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			        return device() ^ now;
		        }()};
		std::uniform_int_distribution<unsigned int> byte(0, 255);

		TransactionId id;
		for (std::uint8_t& b: id.bytes)
		{
			b = static_cast<std::uint8_t>(byte(engine));
		}
		return id;
	}

	std::array<std::uint8_t, kHeaderSize> BuildBindingRequest(const TransactionId& id)
	{
		std::array<std::uint8_t, kHeaderSize> out{};
		WriteU16(out.data(), kBindingRequest);
		WriteU16(out.data() + 2, 0); // no attributes
		WriteU32(out.data() + 4, kMagicCookie);
		for (std::size_t i = 0; i < kTransactionIdSize; ++i)
		{
			out[8 + i] = id.bytes[i];
		}
		return out;
	}

	MessageKind Classify(std::span<const std::byte> datagram)
	{
		if (!LooksLikeStun(datagram))
		{
			return MessageKind::Other;
		}
		switch (ReadU16(Bytes(datagram)))
		{
			case kBindingRequest:
				return MessageKind::BindingRequest;
			case kBindingSuccess:
				return MessageKind::BindingSuccess;
			default:
				return MessageKind::Other;
		}
	}

	std::optional<TransactionId> ReadTransactionId(std::span<const std::byte> datagram)
	{
		if (!LooksLikeStun(datagram))
		{
			return std::nullopt;
		}
		const std::uint8_t* p = Bytes(datagram);
		TransactionId id;
		for (std::size_t i = 0; i < kTransactionIdSize; ++i)
		{
			id.bytes[i] = p[8 + i];
		}
		return id;
	}

	std::array<std::uint8_t, kBindingResponseSize> BuildBindingResponse(const TransactionId& id, const Endpoint& reflexive)
	{
		std::array<std::uint8_t, kBindingResponseSize> out{};
		WriteU16(out.data(), kBindingSuccess);
		WriteU16(out.data() + 2, 12); // one XOR-MAPPED-ADDRESS attribute
		WriteU32(out.data() + 4, kMagicCookie);
		for (std::size_t i = 0; i < kTransactionIdSize; ++i)
		{
			out[8 + i] = id.bytes[i];
		}

		// Only XOR-MAPPED-ADDRESS is sent. Emitting the plain attribute as well would
		// hand a NAT that rewrites payload addresses something to corrupt, for the
		// benefit of nothing that is going to talk to a peer of ours.
		WriteU16(out.data() + 20, kAttrXorMappedAddress);
		WriteU16(out.data() + 22, 8);
		out[24] = 0x00;
		out[25] = kFamilyIpv4;
		WriteU16(out.data() + 26, static_cast<std::uint16_t>(reflexive.port ^ static_cast<std::uint16_t>(kMagicCookie >> 16)));
		WriteU32(out.data() + 28, reflexive.address ^ kMagicCookie);
		return out;
	}

	bool LooksLikeStun(std::span<const std::byte> datagram)
	{
		if (datagram.size() < kHeaderSize)
		{
			return false;
		}
		const std::uint8_t* p = Bytes(datagram);
		// RFC 5389 section 6: the two most significant bits are zero on every STUN
		// message, which is what separates it from other traffic on a shared socket.
		if ((p[0] & 0xC0) != 0)
		{
			return false;
		}
		return ReadU32(p + 4) == kMagicCookie;
	}

	std::optional<Endpoint> ParseBindingResponse(std::span<const std::byte> datagram, const TransactionId& expected)
	{
		if (!LooksLikeStun(datagram))
		{
			return std::nullopt;
		}
		const std::uint8_t* p = Bytes(datagram);
		if (ReadU16(p) != kBindingSuccess)
		{
			return std::nullopt;
		}
		for (std::size_t i = 0; i < kTransactionIdSize; ++i)
		{
			if (p[8 + i] != expected.bytes[i])
			{
				return std::nullopt;
			}
		}

		// The header's length field is authoritative for where the attributes end; a
		// datagram can carry trailing bytes, and walking past the declared length would
		// read them as attributes.
		const std::size_t declared = ReadU16(p + 2);
		const std::size_t available = datagram.size() - kHeaderSize;
		const std::size_t end = kHeaderSize + (declared < available ? declared : available);

		std::optional<Endpoint> plain;
		std::size_t cursor = kHeaderSize;
		while (cursor + 4 <= end)
		{
			const std::uint16_t type = ReadU16(p + cursor);
			const std::size_t length = ReadU16(p + cursor + 2);
			const std::size_t valueAt = cursor + 4;
			if (valueAt + length > end)
			{
				break; // truncated attribute: stop rather than read past the message
			}

			// An address attribute is 1 reserved byte, 1 family byte, 2 port, 4 address.
			if ((type == kAttrXorMappedAddress || type == kAttrMappedAddress) && length >= 8 && p[valueAt + 1] == kFamilyIpv4)
			{
				const std::uint16_t rawPort = ReadU16(p + valueAt + 2);
				const std::uint32_t rawAddress = ReadU32(p + valueAt + 4);
				if (type == kAttrXorMappedAddress)
				{
					// Port is XORed with the cookie's high half, address with all of it.
					return Endpoint{rawAddress ^ kMagicCookie, static_cast<std::uint16_t>(rawPort ^ static_cast<std::uint16_t>(kMagicCookie >> 16))};
				}
				plain = Endpoint{rawAddress, rawPort};
			}

			// Attributes are padded to a 4-byte boundary, and the padding is not counted
			// in the length - walking by the length alone desynchronises the whole list.
			cursor = valueAt + ((length + 3) & ~std::size_t{3});
		}

		return plain;
	}
} // namespace aether::net::stun
