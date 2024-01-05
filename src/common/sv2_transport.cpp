// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/sv2_transport.h>

#include <memusage.h>
#include <common/sv2_messages.h>
#include <common/sv2_noise.h>
#include <random.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/vector.h>

// TODO: drop after https://github.com/bitcoin/bitcoin/pull/28455
CKey GenerateRandomKey() noexcept
{
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    return key;
}

Sv2Transport::Sv2Transport(bool initiating, CKey static_key) noexcept
    : m_cipher{Sv2Cipher(std::move(static_key), initiating)}, m_initiating{initiating},
      m_recv_state{initiating ? RecvState::HANDSHAKE_STEP_2 : RecvState::HANDSHAKE_STEP_1},
      m_send_state{initiating ? SendState::HANDSHAKE_STEP_1 : SendState::HANDSHAKE_STEP_2},
      m_message{Sv2NetMsg(Sv2NetHeader{})}
{
    /** Start sending immediately if we're the initiator of the connection.
        This only happens in test code.
    */
    if (initiating) {
        LOCK(m_send_mutex);
        StartSendingHandshake();
    } else {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Noise session state -> HANDSHAKE_STEP_1\n");
    }
}

void Sv2Transport::SetReceiveState(RecvState recv_state) noexcept
{
    AssertLockHeld(m_recv_mutex);
    // Enforce allowed state transitions.
    switch (m_recv_state) {
    case RecvState::HANDSHAKE_STEP_1:
        Assume(recv_state == RecvState::HANDSHAKE_STEP_2);
        break;
    case RecvState::HANDSHAKE_STEP_2:
        Assume(recv_state == RecvState::APP);
        break;
    case RecvState::APP:
        Assume(recv_state == RecvState::APP_READY);
        break;
    case RecvState::APP_READY:
        Assume(recv_state == RecvState::APP);
        break;
    }
    // Change state.
    m_recv_state = recv_state;
}

void Sv2Transport::SetSendState(SendState send_state) noexcept
{
    AssertLockHeld(m_send_mutex);
    // Enforce allowed state transitions.
    switch (m_send_state) {
    case SendState::HANDSHAKE_STEP_1:
        Assume(send_state == SendState::HANDSHAKE_STEP_2);
        break;
    case SendState::HANDSHAKE_STEP_2:
        Assume(send_state == SendState::READY);
        break;
    case SendState::READY:
        Assume(false); // Final state
        break;
    }
    // Change state.
    m_send_state = send_state;
}

void Sv2Transport::StartSendingHandshake() noexcept
{
    AssertLockHeld(m_send_mutex);
    AssertLockNotHeld(m_recv_mutex);
    Assume(m_send_state == SendState::HANDSHAKE_STEP_1);
    Assume(m_send_buffer.empty());

    m_send_buffer.resize(KEY_SIZE);
    Assume(m_cipher.m_handshake_state);
    m_cipher.m_handshake_state->WriteMsgEphemeralPK(MakeWritableByteSpan(m_send_buffer));

    m_send_state = SendState::HANDSHAKE_STEP_2;
}

void Sv2Transport::SendHandshakeReply() noexcept
{
    AssertLockHeld(m_send_mutex);
    AssertLockHeld(m_recv_mutex);
    Assume(m_send_state == SendState::HANDSHAKE_STEP_2);

    Assume(m_send_buffer.empty());
    m_send_buffer.resize(INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH);
    Assume(m_cipher.m_handshake_state);
    m_cipher.m_handshake_state->WriteMsgES(MakeWritableByteSpan(m_send_buffer));

    m_cipher.FinishHandshake();

    // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "m_hash: %s\n", HexStr(m_cipher.m_hash));

    // We can send and receive stuff now, unless the other side hangs up
    m_send_state = SendState::READY;
    Assume(m_recv_state == RecvState::HANDSHAKE_STEP_2);
    m_recv_state = RecvState::APP;
}

Transport::BytesToSend Sv2Transport::GetBytesToSend(bool have_next_message) const noexcept
{
    AssertLockNotHeld(m_send_mutex);
    LOCK(m_send_mutex);

    Assume(m_send_pos <= m_send_buffer.size());
    return {
        Span{m_send_buffer}.subspan(m_send_pos),
        // We only have more to send after the current m_send_buffer if there is a (next)
        // message to be sent, and we're capable of sending packets. */
        have_next_message && m_send_state == SendState::READY,
        m_send_type
    };
}

void Sv2Transport::MarkBytesSent(size_t bytes_sent) noexcept
{
    AssertLockNotHeld(m_send_mutex);
    LOCK(m_send_mutex);

    // if (m_send_state == SendState::AWAITING_KEY && m_send_pos == 0 && bytes_sent > 0) {
    //     LogPrint(BCLog::NET, "start sending v2 handshake to peer=%d\n", m_nodeid);
    // }

    m_send_pos += bytes_sent;
    Assume(m_send_pos <= m_send_buffer.size());
    // Wipe the buffer when everything is sent.
    if (m_send_pos == m_send_buffer.size()) {
        m_send_pos = 0;
        ClearShrink(m_send_buffer);
    }
}

bool Sv2Transport::SetMessageToSend(CSerializedNetMsg& msg) noexcept
{
    AssertLockNotHeld(m_send_mutex);
    assert(false);
    return false;
}

bool Sv2Transport::SetMessageToSend(Sv2NetMsg& msg) noexcept
{
    AssertLockNotHeld(m_send_mutex);
    LOCK(m_send_mutex);

    // We only allow adding a new message to be sent when in the READY state (so the packet cipher
    // is available) and the send buffer is empty. This limits the number of messages in the send
    // buffer to just one, and leaves the responsibility for queueing them up to the caller.
    if (!(m_send_state == SendState::READY && m_send_buffer.empty())) return false;

    // Construct ciphertext in send buffer.
    const size_t encrypted_msg_size = Sv2NoiseSession::EncryptedMessageSize(msg.m_msg.size());
    m_send_buffer.resize(SV2_HEADER_ENCRYPTED_SIZE + encrypted_msg_size);
    Span<std::byte> buffer_span{MakeWritableByteSpan(m_send_buffer)};

    // Header
    DataStream ss_header_plain{};
    ss_header_plain << msg.m_sv2_header;
    LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Header: %s\n", HexStr(ss_header_plain));
    Span<std::byte> header_encrypted{buffer_span.subspan(0, SV2_HEADER_ENCRYPTED_SIZE)};
    m_cipher.EncryptMessage(ss_header_plain, header_encrypted);
    LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Crypted header: %s\n", HexStr(header_encrypted));

    // Payload
    Span<std::byte> payload_plain = MakeWritableByteSpan(msg.m_msg);
    // TODO: truncate very long messages, about 100 bytes at the start and end
    //       is probably enough for most debugging.
    // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Payload: %s\n", HexStr(payload_plain));
    Span<std::byte> payload_encrypted{buffer_span.subspan(SV2_HEADER_ENCRYPTED_SIZE, encrypted_msg_size)};
    m_cipher.EncryptMessage(payload_plain, payload_encrypted);

    // Release memory
    ClearShrink(msg.m_msg);

    return true;
}

size_t Sv2Transport::GetSendMemoryUsage() const noexcept
{
    AssertLockNotHeld(m_send_mutex);
    LOCK(m_send_mutex);

    return sizeof(m_send_buffer) + memusage::DynamicUsage(m_send_buffer);
}

bool Sv2Transport::ReceivedBytes(Span<const uint8_t>& msg_bytes) noexcept
{
    AssertLockNotHeld(m_send_mutex);
    AssertLockNotHeld(m_recv_mutex);
    /** How many bytes to allocate in the receive buffer at most above what is received so far. */
    static constexpr size_t MAX_RESERVE_AHEAD = 256 * 1024; // TODO: reduce to NOISE_MAX_CHUNK_SIZE?

    LOCK(m_recv_mutex);
    // Process the provided bytes in msg_bytes in a loop. In each iteration a nonzero number of
    // bytes (decided by GetMaxBytesToProcess) are taken from the beginning om msg_bytes, and
    // appended to m_recv_buffer. Then, depending on the receiver state, one of the
    // ProcessReceived*Bytes functions is called to process the bytes in that buffer.
    while (!msg_bytes.empty()) {
        // Decide how many bytes to copy from msg_bytes to m_recv_buffer.
        size_t max_read = GetMaxBytesToProcess();

        // Reserve space in the buffer if there is not enough.
        if (m_recv_buffer.size() + std::min(msg_bytes.size(), max_read) > m_recv_buffer.capacity()) {
            switch (m_recv_state) {
            case RecvState::HANDSHAKE_STEP_1:
                m_recv_buffer.reserve(KEY_SIZE);
                break;
            case RecvState::HANDSHAKE_STEP_2:
                m_recv_buffer.reserve(INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH);
                break;
            case RecvState::APP: {
                // During states where a packet is being received, as much as is expected but never
                // more than MAX_RESERVE_AHEAD bytes in addition to what is received so far.
                // This means attackers that want to cause us to waste allocated memory are limited
                // to MAX_RESERVE_AHEAD above the largest allowed message contents size, and to
                // MAX_RESERVE_AHEAD more than they've actually sent us.
                size_t alloc_add = std::min(max_read, msg_bytes.size() + MAX_RESERVE_AHEAD);
                m_recv_buffer.reserve(m_recv_buffer.size() + alloc_add);
                break;
            }
            case RecvState::APP_READY:
                // The buffer is empty in this state.
                Assume(m_recv_buffer.empty());
                break;
            }
        }

        // Can't read more than provided input.
        max_read = std::min(msg_bytes.size(), max_read);
        // Copy data to buffer.
        m_recv_buffer.insert(m_recv_buffer.end(), UCharCast(msg_bytes.data()), UCharCast(msg_bytes.data() + max_read));
        msg_bytes = msg_bytes.subspan(max_read);

        // Process data in the buffer.
        switch (m_recv_state) {

        case RecvState::HANDSHAKE_STEP_1:
            if (!ProcessReceivedEphemeralKeyBytes()) return false;
            break;

        case RecvState::HANDSHAKE_STEP_2:
            if (!ProcessReceivedHandshakeReplyBytes()) return false;
            break;

        case RecvState::APP:
            if (!ProcessReceivedPacketBytes()) return false;
            break;

        case RecvState::APP_READY:
            return true;

        }
        // Make sure we have made progress before continuing.
        Assume(max_read > 0);
    }

    return true;
}

bool Sv2Transport::ProcessReceivedEphemeralKeyBytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    AssertLockNotHeld(m_send_mutex);
    Assume(m_recv_state == RecvState::HANDSHAKE_STEP_1);
    Assume(m_recv_buffer.size() <= KEY_SIZE);

    if (m_recv_buffer.size() == KEY_SIZE) {
        // Other side's key has been fully received, and can now be Diffie-Hellman
        // combined with our key. This is act 1 of the Noise Protocol handshake.
        // TODO handle failure
        // TODO: MakeByteSpan instead of MakeWritableByteSpan
        Assume(m_cipher.m_handshake_state);
        m_cipher.m_handshake_state->ReadMsgEphemeralPK(MakeWritableByteSpan(m_recv_buffer));
        m_recv_buffer.clear();
        SetReceiveState(RecvState::HANDSHAKE_STEP_2);

        LOCK(m_send_mutex);
        Assume(m_send_buffer.size() == 0);

        // Send our act 2 handshake
        SendHandshakeReply();
    } else {
        // We still have to receive more key bytes.
    }
    return true;
}

bool Sv2Transport::ProcessReceivedHandshakeReplyBytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    AssertLockNotHeld(m_send_mutex);
    Assume(m_recv_state == RecvState::HANDSHAKE_STEP_2);
    Assume(m_recv_buffer.size() <= INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH);

    if (m_recv_buffer.size() == INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH) {
        // TODO handle failure
        // TODO: MakeByteSpan instead of MakeWritableByteSpan
        Assume(m_cipher.m_handshake_state);
        bool res = m_cipher.m_handshake_state->ReadMsgES(MakeWritableByteSpan(m_recv_buffer));
        if (!res) return false;
        m_recv_buffer.clear();
        m_cipher.FinishHandshake();
        SetReceiveState(RecvState::APP);

        LOCK(m_send_mutex);
        Assume(m_send_buffer.size() == 0);

        SetSendState(SendState::READY);
    } else {
        // We still have to receive more key bytes.
    }
    return true;
}

size_t Sv2Transport::GetMaxBytesToProcess() noexcept
{
    AssertLockHeld(m_recv_mutex);
    switch (m_recv_state) {
    case RecvState::HANDSHAKE_STEP_1:
        // In this state, we only allow the 32-byte key into the receive buffer.
        Assume(m_recv_buffer.size() <= KEY_SIZE);
        return KEY_SIZE - m_recv_buffer.size();
    case RecvState::HANDSHAKE_STEP_2:
        // In this state, we only allow the handshake reply into the receive buffer.
        Assume(m_recv_buffer.size() <= INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH);
        return INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH - m_recv_buffer.size();
    case RecvState::APP:
        // Decode a packet. Process the header first,
        // so that we know where the current packet ends (and we don't process bytes from the next
        // packet yet). Then, process the ciphertext bytes of the current packet.
        if (m_recv_buffer.size() < SV2_HEADER_ENCRYPTED_SIZE) {
            return SV2_HEADER_ENCRYPTED_SIZE - m_recv_buffer.size();
        } else {
            // When transitioning from receiving the packet length to receiving its ciphertext,
            // the encrypted header is left in the receive buffer.
            size_t expanded_size_with_header = SV2_HEADER_ENCRYPTED_SIZE + Sv2NoiseSession::EncryptedMessageSize(m_header.m_msg_len);
            return expanded_size_with_header - m_recv_buffer.size();
        }
    case RecvState::APP_READY:
        // No bytes can be processed until GetMessage() is called.
        return 0;
    }
    Assume(false); // unreachable
    return 0;
}

bool Sv2Transport::ProcessReceivedPacketBytes() noexcept
{
    AssertLockHeld(m_recv_mutex);
    Assume(m_recv_state == RecvState::APP);

    // The maximum permitted decrypted payload size for a packet
    static constexpr size_t MAX_CONTENTS_LEN = 16777215; // 24 bit unsigned;

    // TODO: can message be zero length?
    Assume(m_recv_buffer.size() <= SV2_HEADER_ENCRYPTED_SIZE || m_header.m_msg_len > 0);

    if (m_recv_buffer.size() == SV2_HEADER_ENCRYPTED_SIZE) {
        // Header received, decrypt it.
        Span<std::byte> header_span = MakeWritableByteSpan(m_recv_buffer);
        if  (!m_cipher.DecryptMessage(header_span)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Failed to decrypt header\n");
            return false;
        }

        Span<std::byte> decrypted_header = header_span.subspan(0, SV2_HEADER_PLAIN_SIZE);
        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Header: %s\n", HexStr(decrypted_header));

        // Decode header
        DataStream ss_header{decrypted_header};
        node::Sv2NetHeader header;
        ss_header >> header;
        m_header = std::move(header);

        // TODO: 16 MB is pretty large, maybe set lower limits for most or all message types?
        if (m_header.m_msg_len > MAX_CONTENTS_LEN) {
            LogPrint(BCLog::SV2, "Packet too large (%u bytes)\n", m_header.m_msg_len);
            return false;
        }
    } else if (m_recv_buffer.size() > SV2_HEADER_ENCRYPTED_SIZE &&
               m_recv_buffer.size() == SV2_HEADER_ENCRYPTED_SIZE + Sv2NoiseSession::EncryptedMessageSize(m_header.m_msg_len)) {
        /** Ciphertext received: decrypt into decode_buffer and deserialize into m_message.
          *
          * Note that it is impossible to reach this branch without hitting the
          * branch above first, as GetMaxBytesToProcess only allows up to
          * SV2_HEADER_ENCRYPTED_SIZE into the buffer before that point. */
        std::vector<uint8_t> decode_buffer;
        decode_buffer.resize(m_header.m_msg_len);

        Span<std::byte> recv_span{MakeWritableByteSpan(m_recv_buffer).subspan(SV2_HEADER_ENCRYPTED_SIZE)};
        if (!m_cipher.DecryptMessage(recv_span)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Failed to decrypt message payload\n");
            return false;
        }
        Span<std::byte> payload = recv_span.subspan(0, m_header.m_msg_len);
        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Payload: %s\n", HexStr(payload));

        std::transform(payload.begin(), payload.end(), decode_buffer.begin(),
                           [](std::byte b) { return static_cast<uint8_t>(b); });

        // Wipe the receive buffer where the next packet will be received into.
        ClearShrink(m_recv_buffer);

        Sv2NetMsg message{std::move(m_header), std::move(decode_buffer)};
        m_message = std::move(message);

        // At this point we have a valid message decrypted into m_message.
        SetReceiveState(RecvState::APP_READY);
    } else {
        // We either have less than 22 bytes, so we don't know the packet's length yet, or more
        // than 22 bytes but less than the packet's full ciphertext. Wait until those arrive.
    }
    return true;
}

bool Sv2Transport::ReceivedMessageComplete() const noexcept
{
    AssertLockNotHeld(m_recv_mutex);
    LOCK(m_recv_mutex);

    return m_recv_state == RecvState::APP_READY;
}

CNetMessage Sv2Transport::GetReceivedMessage(std::chrono::microseconds time, bool& reject_message) noexcept
{
    AssertLockNotHeld(m_recv_mutex);
    assert(false);
    return CNetMessage{DataStream{}};
}

Sv2NetMsg&& Sv2Transport::GetReceivedMessage() noexcept
{
    AssertLockNotHeld(m_recv_mutex);
    LOCK(m_recv_mutex);
    Assume(m_recv_state == RecvState::APP_READY);

    SetReceiveState(RecvState::APP);
    return std::move(m_message);
}

Transport::Info Sv2Transport::GetInfo() const noexcept
{
    return {.transport_type = TransportProtocolType::V1, .session_id = {}};
}

std::string RecvStateAsString(Sv2Transport::RecvState state)
{
    switch (state) {
    case Sv2Transport::RecvState::HANDSHAKE_STEP_1:
        return "HANDSHAKE_STEP_1";
    case Sv2Transport::RecvState::HANDSHAKE_STEP_2:
        return "HANDSHAKE_STEP_2";
    case Sv2Transport::RecvState::APP:
        return "APP";
    case Sv2Transport::RecvState::APP_READY:
        return "APP_READY";
    } // no default case, so the compiler can warn about missing cases

    assert(false);
}

std::string SendStateAsString(Sv2Transport::SendState state)
{
    switch (state) {
    case Sv2Transport::SendState::HANDSHAKE_STEP_1:
        return "HANDSHAKE_STEP_1";
    case Sv2Transport::SendState::HANDSHAKE_STEP_2:
        return "HANDSHAKE_STEP_2";
    case Sv2Transport::SendState::READY:
        return "READY";
    } // no default case, so the compiler can warn about missing cases

    assert(false);
}