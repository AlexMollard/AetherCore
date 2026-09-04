#include "net/StunAttributes.hpp"

#include <algorithm>

namespace aether::net::stun
{
	namespace
	{
		constexpr std::uint8_t kFamilyIpv4 = 0x01;
		constexpr std::uint16_t kHmacSha1Size = 20; // crypto::Sha1Digest::bytes.size()

		// Everything on the wire is big-endian; saying so at each read beats a single
		// byte-swap at some boundary every later reader has to remember exists.
		std::uint16_t ReadU16(const std::uint8_t* p)
		{
			return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
		}

		std::uint32_t ReadU32(const std::uint8_t* p)
		{
			return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
		}

		// RFC 5389 s6: the 14-bit message type interleaves a 2-bit class into a 12-bit
		// method rather than placing them in contiguous fields. Method values below 16
		// (every method this layer defines) fit entirely in the low nibble, so a naive
		// "method in the low byte, class shifted above it" encoding happens to produce
		// the right bits for a Request (class 0) and silently wrong bits for anything
		// else - Binding's own Success Response (0x0101, not 0x0102) already exercises
		// that trap, which is why StunMessage.cpp hardcodes it rather than deriving it.
		std::uint16_t PackMessageType(Method method, MessageClass cls)
		{
			const auto m = static_cast<std::uint16_t>(method);
			const auto c = static_cast<std::uint16_t>(cls);
			return static_cast<std::uint16_t>(((m & 0x0F80u) << 2) | ((m & 0x0070u) << 1) | (m & 0x000Fu) | ((c & 0x02u) << 7) | ((c & 0x01u) << 4));
		}

		std::pair<Method, MessageClass> UnpackMessageType(std::uint16_t type)
		{
			const auto m = static_cast<std::uint16_t>(((type & 0x3E00u) >> 2) | ((type & 0x00E0u) >> 1) | (type & 0x000Fu));
			const auto c = static_cast<std::uint16_t>(((type & 0x0100u) >> 7) | ((type & 0x0010u) >> 4));
			return {static_cast<Method>(m), static_cast<MessageClass>(c)};
		}
	} // namespace

	MessageBuilder::MessageBuilder(Method method, MessageClass cls, const TransactionId& id)
	{
		m_bytes.assign(kHeaderSize, 0);
		const std::uint16_t type = PackMessageType(method, cls);
		m_bytes[0] = static_cast<std::uint8_t>(type >> 8);
		m_bytes[1] = static_cast<std::uint8_t>(type & 0xFF);
		// bytes[2..3] (the length field) stay zero until the first attribute is added.
		m_bytes[4] = static_cast<std::uint8_t>(kMagicCookie >> 24);
		m_bytes[5] = static_cast<std::uint8_t>((kMagicCookie >> 16) & 0xFF);
		m_bytes[6] = static_cast<std::uint8_t>((kMagicCookie >> 8) & 0xFF);
		m_bytes[7] = static_cast<std::uint8_t>(kMagicCookie & 0xFF);
		for (std::size_t i = 0; i < kTransactionIdSize; ++i)
		{
			m_bytes[8 + i] = id.bytes[i];
		}
	}

	void MessageBuilder::SetLengthField(std::uint16_t attributesLength)
	{
		m_bytes[2] = static_cast<std::uint8_t>(attributesLength >> 8);
		m_bytes[3] = static_cast<std::uint8_t>(attributesLength & 0xFF);
	}

	void MessageBuilder::AppendAttributeHeader(Attribute a, std::uint16_t length)
	{
		const auto type = static_cast<std::uint16_t>(a);
		m_bytes.push_back(static_cast<std::uint8_t>(type >> 8));
		m_bytes.push_back(static_cast<std::uint8_t>(type & 0xFF));
		m_bytes.push_back(static_cast<std::uint8_t>(length >> 8));
		m_bytes.push_back(static_cast<std::uint8_t>(length & 0xFF));
	}

	void MessageBuilder::PadToAlignment()
	{
		while (m_bytes.size() % 4 != 0)
		{
			m_bytes.push_back(0x00);
		}
	}

	void MessageBuilder::AddU32(Attribute a, std::uint32_t value)
	{
		const std::array<std::byte, 4> raw{
			static_cast<std::byte>((value >> 24) & 0xFF),
			static_cast<std::byte>((value >> 16) & 0xFF),
			static_cast<std::byte>((value >> 8) & 0xFF),
			static_cast<std::byte>(value & 0xFF),
		};
		AddBytes(a, raw);
	}

	void MessageBuilder::AddBytes(Attribute a, std::span<const std::byte> value)
	{
		// The value's real length is what goes in the TLV header; padding added below
		// to reach the next 4-byte boundary is deliberately not counted in it, matching
		// how every reader (this one included) must interpret the length on the wire.
		AppendAttributeHeader(a, static_cast<std::uint16_t>(value.size()));
		const auto* p = reinterpret_cast<const std::uint8_t*>(value.data());
		m_bytes.insert(m_bytes.end(), p, p + value.size());
		PadToAlignment();
		SetLengthField(static_cast<std::uint16_t>(m_bytes.size() - kHeaderSize));
	}

	void MessageBuilder::AddText(Attribute a, std::string_view value)
	{
		const std::span<const char> chars(value.data(), value.size());
		AddBytes(a, std::as_bytes(chars));
	}

	void MessageBuilder::AddXorAddress(Attribute a, const Endpoint& endpoint)
	{
		const std::uint16_t xorPort = static_cast<std::uint16_t>(endpoint.port ^ (kMagicCookie >> 16));
		const std::uint32_t xorAddress = endpoint.address ^ kMagicCookie;
		const std::array<std::byte, 8> raw{
			std::byte{0x00}, // reserved
			std::byte{kFamilyIpv4},
			static_cast<std::byte>(xorPort >> 8),
			static_cast<std::byte>(xorPort & 0xFF),
			static_cast<std::byte>((xorAddress >> 24) & 0xFF),
			static_cast<std::byte>((xorAddress >> 16) & 0xFF),
			static_cast<std::byte>((xorAddress >> 8) & 0xFF),
			static_cast<std::byte>(xorAddress & 0xFF),
		};
		AddBytes(a, raw);
	}

	void MessageBuilder::AppendMessageIntegrity(const crypto::Md5Digest& key)
	{
		// RFC 5389 s15.4: the HMAC covers the message as if this attribute (4-byte
		// header + 20-byte HMAC-SHA1 value) were already the last thing in it, so the
		// length field is patched to that size before hashing rather than left at
		// whatever it was before this call.
		constexpr std::uint16_t kAttributeSize = 4 + 20;
		SetLengthField(static_cast<std::uint16_t>(m_bytes.size() - kHeaderSize + kAttributeSize));

		const std::span<const std::byte> keyBytes = std::as_bytes(std::span{key.bytes});
		const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(m_bytes.data()), m_bytes.size());
		const crypto::Sha1Digest hmac = crypto::HmacSha1(keyBytes, data);

		AppendAttributeHeader(Attribute::MessageIntegrity, static_cast<std::uint16_t>(hmac.bytes.size()));
		m_bytes.insert(m_bytes.end(), hmac.bytes.begin(), hmac.bytes.end());
		// 20 bytes is already a multiple of 4: no padding, and the length field already
		// matches what was patched above, but setting it again keeps this call
		// order-independent of any future attribute this function might grow to add.
		SetLengthField(static_cast<std::uint16_t>(m_bytes.size() - kHeaderSize));
	}

	void MessageBuilder::AppendFingerprint()
	{
		constexpr std::uint16_t kAttributeSize = 4 + 4;
		SetLengthField(static_cast<std::uint16_t>(m_bytes.size() - kHeaderSize + kAttributeSize));

		const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(m_bytes.data()), m_bytes.size());
		const std::uint32_t crc = crypto::Crc32(data) ^ 0x5354554Eu;
		const std::array<std::byte, 4> value{
			static_cast<std::byte>((crc >> 24) & 0xFF),
			static_cast<std::byte>((crc >> 16) & 0xFF),
			static_cast<std::byte>((crc >> 8) & 0xFF),
			static_cast<std::byte>(crc & 0xFF),
		};

		AppendAttributeHeader(Attribute::Fingerprint, static_cast<std::uint16_t>(value.size()));
		const auto* p = reinterpret_cast<const std::uint8_t*>(value.data());
		m_bytes.insert(m_bytes.end(), p, p + value.size());
		SetLengthField(static_cast<std::uint16_t>(m_bytes.size() - kHeaderSize));
	}

	std::span<const std::uint8_t> MessageBuilder::Bytes() const
	{
		return m_bytes;
	}

	std::optional<MessageReader> MessageReader::Parse(std::span<const std::byte> datagram)
	{
		if (datagram.size() < kHeaderSize)
		{
			return std::nullopt; // too short to even hold a header
		}

		const auto* p = reinterpret_cast<const std::uint8_t*>(datagram.data());
		const std::uint16_t type = ReadU16(p);
		if ((type & 0xC000u) != 0)
		{
			return std::nullopt; // the top two bits of a STUN message type are always zero
		}

		const std::uint16_t declaredLength = ReadU16(p + 2);
		if (declaredLength % 4 != 0)
		{
			return std::nullopt; // a real message's attribute region is always 4-byte aligned
		}
		if (static_cast<std::size_t>(kHeaderSize) + declaredLength > datagram.size())
		{
			return std::nullopt; // claims more than the datagram actually holds
		}
		if (ReadU32(p + 4) != kMagicCookie)
		{
			return std::nullopt;
		}

		MessageReader reader;
		reader.m_datagram = datagram.subspan(0, kHeaderSize + declaredLength);
		const auto unpacked = UnpackMessageType(type);
		reader.m_method = unpacked.first;
		reader.m_class = unpacked.second;
		for (std::size_t i = 0; i < kTransactionIdSize; ++i)
		{
			reader.m_transactionId.bytes[i] = p[8 + i];
		}

		std::size_t cursor = kHeaderSize;
		const std::size_t regionEnd = kHeaderSize + declaredLength;
		while (cursor < regionEnd)
		{
			if (regionEnd - cursor < 4)
			{
				return std::nullopt; // a stray partial attribute header, not a clean end
			}
			const std::uint16_t attrType = ReadU16(p + cursor);
			const std::uint16_t attrLength = ReadU16(p + cursor + 2);
			const std::size_t valueOffset = cursor + 4;
			if (valueOffset + attrLength > regionEnd)
			{
				return std::nullopt; // the length claims more of the datagram than it owns
			}
			reader.m_attributes.push_back(AttributeSpan{attrType, static_cast<std::uint32_t>(valueOffset), attrLength});

			// Padding to the next 4-byte boundary is not counted in attrLength, but it
			// still has to fit inside the region or the next attribute's header would be
			// read out of the padding bytes of this one.
			const std::size_t padded = (static_cast<std::size_t>(attrLength) + 3) & ~static_cast<std::size_t>(3);
			if (valueOffset + padded > regionEnd)
			{
				return std::nullopt;
			}
			cursor = valueOffset + padded;
		}
		if (cursor != regionEnd)
		{
			return std::nullopt; // unreachable given the checks above, kept as a hard invariant
		}

		return reader;
	}

	Method MessageReader::GetMethod() const
	{
		return m_method;
	}

	MessageClass MessageReader::GetClass() const
	{
		return m_class;
	}

	const TransactionId& MessageReader::GetTransactionId() const
	{
		return m_transactionId;
	}

	std::optional<std::span<const std::byte>> MessageReader::Find(Attribute a) const
	{
		const auto type = static_cast<std::uint16_t>(a);
		for (const auto& attr: m_attributes)
		{
			if (attr.type == type)
			{
				return m_datagram.subspan(attr.offset, attr.length);
			}
		}
		return std::nullopt;
	}

	std::optional<std::uint32_t> MessageReader::U32(Attribute a) const
	{
		const auto value = Find(a);
		if (!value || value->size() != 4)
		{
			return std::nullopt;
		}
		return ReadU32(reinterpret_cast<const std::uint8_t*>(value->data()));
	}

	std::optional<std::string_view> MessageReader::Text(Attribute a) const
	{
		const auto value = Find(a);
		if (!value)
		{
			return std::nullopt;
		}
		return std::string_view(reinterpret_cast<const char*>(value->data()), value->size());
	}

	std::optional<Endpoint> MessageReader::XorAddress(Attribute a) const
	{
		const auto value = Find(a);
		if (!value || value->size() < 2)
		{
			return std::nullopt;
		}
		const auto* p = reinterpret_cast<const std::uint8_t*>(value->data());
		if (p[1] != kFamilyIpv4)
		{
			return std::nullopt; // IPv6 (or anything else): rejected, never misread as IPv4
		}
		if (value->size() != 8)
		{
			return std::nullopt; // an IPv4 XOR address attribute is always exactly 8 bytes
		}

		Endpoint endpoint;
		endpoint.port = static_cast<std::uint16_t>(ReadU16(p + 2) ^ (kMagicCookie >> 16));
		endpoint.address = ReadU32(p + 4) ^ kMagicCookie;
		return endpoint;
	}

	std::optional<std::pair<int, std::string_view>> MessageReader::ErrorCode() const
	{
		const auto value = Find(Attribute::ErrorCode);
		if (!value || value->size() < 4)
		{
			return std::nullopt;
		}
		const auto* p = reinterpret_cast<const std::uint8_t*>(value->data());
		const int errorClass = p[2] & 0x07;
		const int number = p[3];
		if (errorClass < 3 || errorClass > 6)
		{
			return std::nullopt; // RFC 5389 s15.6: the class digit is always 3-6
		}
		const std::string_view reason(reinterpret_cast<const char*>(value->data()) + 4, value->size() - 4);
		return std::make_pair(errorClass * 100 + number, reason);
	}

	bool MessageReader::VerifyMessageIntegrity(const crypto::Md5Digest& key) const
	{
		const auto type = static_cast<std::uint16_t>(Attribute::MessageIntegrity);
		for (const auto& attr: m_attributes)
		{
			if (attr.type != type)
			{
				continue;
			}
			if (attr.length != kHmacSha1Size)
			{
				return false; // truncated, or simply not an HMAC-SHA1 output
			}

			// The HMAC covers [0, headerStart) with the length field patched to the
			// message's length at the moment this attribute was appended - i.e.
			// excluding this attribute's own bytes and anything (such as FINGERPRINT)
			// added after it. `attr.offset` is the value's offset; the header
			// (type + length, 4 bytes) sits immediately before it.
			const std::size_t headerStart = attr.offset - 4;
			std::vector<std::byte> prefix(m_datagram.begin(), m_datagram.begin() + static_cast<std::ptrdiff_t>(headerStart));
			const auto adjustedLength = static_cast<std::uint16_t>(headerStart - kHeaderSize + 4 + attr.length);
			prefix[2] = static_cast<std::byte>(adjustedLength >> 8);
			prefix[3] = static_cast<std::byte>(adjustedLength & 0xFF);

			const std::span<const std::byte> keyBytes = std::as_bytes(std::span{key.bytes});
			const crypto::Sha1Digest expected = crypto::HmacSha1(keyBytes, prefix);
			const auto* actual = reinterpret_cast<const std::uint8_t*>(m_datagram.data() + attr.offset);
			return std::equal(expected.bytes.begin(), expected.bytes.end(), actual);
		}
		return false; // no MESSAGE-INTEGRITY attribute present
	}

	bool LooksLikeChannelData(std::span<const std::byte> datagram)
	{
		if (datagram.size() < kChannelDataHeaderSize)
		{
			return false;
		}
		const auto first = static_cast<std::uint8_t>(datagram[0]);
		return (first & 0xC0) == 0x40; // channel numbers occupy 0x4000-0x7FFF
	}
} // namespace aether::net::stun
