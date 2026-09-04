#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "net/StunAuth.hpp"
#include "net/StunMessage.hpp"

namespace aether::net::stun
{
	// The general STUN/TURN message layer that sits beside the Binding-only client in
	// StunMessage.hpp. TURN (RFC 5766 / RFC 8656) reuses the same 20-byte header and TLV
	// attribute framing for Allocate/Refresh/CreatePermission/ChannelBind, but needs
	// methods, attributes, and long-term-credential authentication that a bare Binding
	// client has no reason to carry - so those live here instead of being bolted onto
	// StunMessage.hpp.
	//
	// Message-only by the same design as StunMessage.hpp: no sockets, so this can be
	// tested without a network, and the same buffers can be handed straight to whatever
	// layer above owns the ENet-shared socket. Every parse path treats its input as
	// coming from an unauthenticated peer: lengths are validated before they are used to
	// size or index anything, and a malformed message is refused outright rather than
	// salvaged as far as it will go.

	// The method a message requests or reports on. Binding is inherited from plain STUN;
	// the rest are RFC 5766 TURN methods layered on the same header.
	enum class Method : std::uint16_t
	{
		Binding = 0x001,
		Allocate = 0x003,
		Refresh = 0x004,
		Send = 0x006,
		Data = 0x007,
		CreatePermission = 0x008,
		ChannelBind = 0x009,
	};

	// What kind of message this is: a request, a fire-and-forget indication, or one of
	// the two kinds of answer to a request.
	enum class MessageClass : std::uint8_t
	{
		Request = 0,
		Indication = 1,
		SuccessResponse = 2,
		ErrorResponse = 3,
	};

	// TLV attribute types this layer has named accessors for. The wire format does not
	// need an attribute's type to be one of these to walk past it correctly - only
	// `Find` and its typed callers need the type to matter.
	enum class Attribute : std::uint16_t
	{
		MappedAddress = 0x0001,
		Username = 0x0006,
		MessageIntegrity = 0x0008,
		ErrorCode = 0x0009,
		UnknownAttributes = 0x000A,
		ChannelNumber = 0x000C,
		Lifetime = 0x000D,
		XorPeerAddress = 0x0012,
		Data = 0x0013,
		Realm = 0x0014,
		Nonce = 0x0015,
		XorRelayedAddress = 0x0016,
		RequestedTransport = 0x0019,
		DontFragment = 0x001A,
		XorMappedAddress = 0x0020,
		ReservationToken = 0x0022,
		Software = 0x8022,
		Fingerprint = 0x8028,
	};

	// Builds a STUN/TURN message one attribute at a time, in the order attributes are
	// added. RFC 5389 s15.4 defines MESSAGE-INTEGRITY as covering everything written
	// before it, so callers add authentication attributes last and MESSAGE-INTEGRITY /
	// FINGERPRINT only after every other attribute is in place.
	class MessageBuilder
	{
	public:
		MessageBuilder(Method method, MessageClass cls, const TransactionId& id);

		void AddU32(Attribute a, std::uint32_t value);
		void AddBytes(Attribute a, std::span<const std::byte> value);
		void AddText(Attribute a, std::string_view value);

		// Port XORed with the top 16 bits of the magic cookie, address XORed with the
		// whole cookie (RFC 5389 s15.2). IPv4 only, matching `Endpoint` and the ENet
		// address type this whole subsystem is built around.
		void AddXorAddress(Attribute a, const Endpoint& endpoint);

		// Appends MESSAGE-INTEGRITY last of the credential attributes (and before
		// FINGERPRINT, if any follows). The HMAC is computed with the header's length
		// field temporarily set as though this 24-byte attribute were already present -
		// hashing the pre-attribute length instead produces a value no real TURN or ICE
		// server will accept.
		void AppendMessageIntegrity(const crypto::Md5Digest& key);

		// Appends FINGERPRINT after everything else, including MESSAGE-INTEGRITY if
		// present. Its CRC32 is taken over the message with the length field set to
		// include this attribute, then XORed with 0x5354554e (RFC 5389 s15.5) so a
		// packet whose trailing bytes coincidentally look like a plain CRC is not
		// mistaken for one carrying the real attribute.
		void AppendFingerprint();

		[[nodiscard]] std::span<const std::uint8_t> Bytes() const;

	private:
		std::vector<std::uint8_t> m_bytes;

		void SetLengthField(std::uint16_t attributesLength);
		void AppendAttributeHeader(Attribute a, std::uint16_t length);
		void PadToAlignment();
	};

	// Parses a received datagram as a STUN/TURN message. `Parse` validates every
	// attribute's framing before returning anything: a datagram that is too short, that
	// claims a message length longer than it actually holds, or whose attribute chain
	// runs past the declared length or the buffer (whichever is shorter) is refused
	// wholesale rather than read as far as it safely can be.
	class MessageReader
	{
	public:
		[[nodiscard]] static std::optional<MessageReader> Parse(std::span<const std::byte> datagram);

		[[nodiscard]] Method GetMethod() const;
		[[nodiscard]] MessageClass GetClass() const;
		[[nodiscard]] const TransactionId& GetTransactionId() const;

		// The attribute's real value, padding excluded. Its bytes alias the datagram
		// `Parse` was called with - the reader carries no attribute past the caller's
		// own buffer lifetime.
		[[nodiscard]] std::optional<std::span<const std::byte>> Find(Attribute a) const;
		[[nodiscard]] std::optional<std::uint32_t> U32(Attribute a) const;
		[[nodiscard]] std::optional<std::string_view> Text(Attribute a) const;

		// IPv4 only: a family byte that says otherwise (IPv6) is refused rather than
		// reinterpreted, since a 16-byte address read as a 4-byte one would silently
		// hand back an address nobody sent.
		[[nodiscard]] std::optional<Endpoint> XorAddress(Attribute a) const;

		[[nodiscard]] std::optional<std::pair<int, std::string_view>> ErrorCode() const;

		// Recomputes the HMAC exactly as `AppendMessageIntegrity` built it: over the
		// bytes preceding this attribute, with the length field patched to the length
		// the message had at the moment MESSAGE-INTEGRITY was appended (i.e. excluding
		// anything - such as FINGERPRINT - added after it). Safe when the attribute is
		// absent or is not exactly the 20 bytes an HMAC-SHA1 output requires.
		[[nodiscard]] bool VerifyMessageIntegrity(const crypto::Md5Digest& key) const;

	private:
		struct AttributeSpan
		{
			std::uint16_t type = 0;
			std::uint32_t offset = 0; // offset of the value (not the TLV header) within m_datagram
			std::uint16_t length = 0; // the value's real length; padding is not part of it
		};

		std::span<const std::byte> m_datagram;
		Method m_method = Method::Binding;
		MessageClass m_class = MessageClass::Request;
		TransactionId m_transactionId;
		std::vector<AttributeSpan> m_attributes;

		MessageReader() = default;
	};

	// RFC 5766 s11: ChannelData is not a STUN message at all - just a 4-byte header
	// (channel number, then length) directly followed by the payload, with no magic
	// cookie and no attributes. Channel numbers occupy 0x4000-0x7FFF, whose top two bits
	// are always `01`; a STUN message's are always `00`. That is the same cheap
	// leading-bits demux `LooksLikeStun` uses for its own purpose, extended so a socket
	// carrying both kinds of TURN traffic can tell them apart before parsing either.
	inline constexpr std::size_t kChannelDataHeaderSize = 4;
	[[nodiscard]] bool LooksLikeChannelData(std::span<const std::byte> datagram);
} // namespace aether::net::stun
