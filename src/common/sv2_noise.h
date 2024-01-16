// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMMON_SV2_NOISE_H
#define BITCOIN_COMMON_SV2_NOISE_H

#include <compat/compat.h>
#include <key.h>
#include <pubkey.h>
#include <random.h>
#include <streams.h>
#include <uint256.h>

/** The Noise Protocol Framework
 *  https://noiseprotocol.org/noise.html
 *  Revision 38, 2018-07-11
 *
 *  Stratum v2 handshake and cipher specification:
 *  https://github.com/stratum-mining/sv2-spec/blob/main/04-Protocol-Security.md
 */

static constexpr size_t POLY1305_TAGLEN{16};
static constexpr size_t KEY_SIZE = 32;
static constexpr size_t ELLSWIFT_KEY_SIZE = 64;
static constexpr size_t ECDH_OUTPUT_SIZE = 32;
/** Section 3: All Noise messages are less than or equal to 65535 bytes in length. */
static constexpr size_t NOISE_MAX_CHUNK_SIZE = 65535;
/** Sv2 spec 4.5.2 */
static constexpr size_t SIGNATURE_NOISE_MESSAGE_SIZE = 2 + 4 + 4 + 64;
static constexpr size_t INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH = ELLSWIFT_KEY_SIZE + ELLSWIFT_KEY_SIZE +
                        POLY1305_TAGLEN + SIGNATURE_NOISE_MESSAGE_SIZE + POLY1305_TAGLEN;

// Sha256 hash of the ascii encoding - "Noise_NX_EllSwiftXonly_ChaChaPoly_SHA256".
// This is the first step required when setting up the chaining key.
const std::vector<uint8_t> PROTOCOL_NAME_HASH = {
    27, 97, 156, 90, 248, 120, 254, 68, 34, 119, 45, 129, 209, 41, 152, 82,
    26,137, 97, 115, 62, 44, 177, 60, 145, 24, 250, 214, 68, 188, 1, 128};

// The double hash of protocol name "Noise_NX_EllSwiftXonly_ChaChaPoly_SHA256".
static std::vector<uint8_t> PROTOCOL_NAME_DOUBLE_HASH = {60, 102, 112, 143, 69, 248, 185, 34, 53, 193, 3, 46, 250, 104, 70, 171,
                                                         139, 103, 55, 191, 199, 9, 77, 179, 99, 170, 7, 240, 219, 36, 226, 71};

/** Simple certificate for the static key signed by the authority key.
  * See 4.5.2 and 4.5.3 of the Stratum v2 spec.
  */
class Sv2SignatureNoiseMessage
{
private:
    uint16_t m_version = 0;
    uint32_t m_valid_from = 0;
    uint32_t m_valid_to = 0;
    std::vector<unsigned char> m_sig;

    uint256 GetHash();
    void SignSchnorr(const CKey& authority_key, Span<unsigned char> sig);

public:
    Sv2SignatureNoiseMessage() = default;
    Sv2SignatureNoiseMessage(uint16_t version, uint32_t valid_from, uint32_t valid_to, const XOnlyPubKey& static_key, const CKey& authority_key);

    /* The certificate serializes pubkeys in x-only format, not EllSwift. */
    XOnlyPubKey m_static_key = {};

    [[ nodiscard ]] bool Validate(XOnlyPubKey authority_key);

    template <typename Stream>
    // The static_key is signed for, but not serialized.
    void Serialize(Stream& s) const
    {
        s << m_version
          << m_valid_from
          << m_valid_to;

        s.write(MakeByteSpan(m_sig));
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        m_sig.resize(64);

        s >> m_version;
        s >> m_valid_from;
        s >> m_valid_to;
        s.read(MakeWritableByteSpan(m_sig));
    }
};

/*
 * The CipherState uses m_key (k) and m_nonce (n) to encrypt and decrypt ciphertexts.
 * During the handshake phase each party has a single CipherState, but during
 * the transport phase each party has two CipherState objects: one for sending,
 * and one for receiving.
 *
 * See chapter "5. Processing rules" of the Noise paper.
 */
class Sv2CipherState
{
public:
    Sv2CipherState() = default;
    explicit Sv2CipherState(uint8_t key[KEY_SIZE]);

    /** Decrypt message
     * @param[in] associated_data associated data
     * @param[in,out] msg message with encrypted and authenticated chunks
     *
     * @returns whether decryption succeeded
     */
     [[nodiscard]] bool DecryptWithAd(Span<const std::byte> associated_data, Span<std::byte> msg);
    void EncryptWithAd(Span<const std::byte> associated_data, Span<std::byte> msg);

    /** The message will be chunked in NOISE_MAX_CHUNK_SIZE parts and expanded
     *  by 16 bytes per chunk for its MAC.
     *
     * @param[in] input     message
     * @param[out] output   message with encrypted and authenticated chunks
     */
    void EncryptMessage(Span<const std::byte> input, Span<std::byte> output);

    /** Decrypt message.
     *
     * @param[in] message     message
     */
    [[ nodiscard ]] bool DecryptMessage(Span<std::byte> message);

private:
    uint8_t m_key[KEY_SIZE];
    uint64_t m_nonce = 0;
};

/*
 * A SymmetricState object contains a CipherState plus m_chaining_key (ck) and
 * m_hash_output (h) variables. It is so-named because it encapsulates all the
 * "symmetric crypto" used by Noise. During the handshake phase each party has
 * a single SymmetricState, which can be deleted once the handshake is finished.
 *
 * See chapter "5. Processing rules" of the Noise paper.
 */
class Sv2SymmetricState
{
public:
    Sv2SymmetricState() {
        std::memcpy(m_chaining_key, PROTOCOL_NAME_HASH.data(), PROTOCOL_NAME_HASH.size());
    }

    void MixHash(const Span<const std::byte> input);
    void MixKey(const Span<const std::byte> input_key_material);
    void EncryptAndHash(Span<std::byte> data);
    [[ nodiscard ]] bool DecryptAndHash(Span<std::byte> data);
    std::array<Sv2CipherState, 2> Split();

    uint256 GetHashOutput();

    /* For testing */
    void LogChainingKey();
    std::string GetChainingKey();

private:
    uint8_t m_chaining_key[KEY_SIZE];
    uint256 m_hash_output = uint256(PROTOCOL_NAME_DOUBLE_HASH);
    Sv2CipherState m_cipher_state;

    void HKDF2(const Span<const std::byte> input_key_material, uint8_t out0[KEY_SIZE], uint8_t out1[KEY_SIZE]);
};

/*
 * A HandshakeState object contains a SymmetricState plus DH variables (s, e, rs, re)
 * and a variable representing the handshake pattern. During the handshake phase
 * each party has a single HandshakeState, which can be deleted once the handshake
 * is finished.
 *
 * See chapter "5. Processing rules" of the Noise paper.
 */

class Sv2HandshakeState
{
public:
    /*
     * If we are the initiator m_authority_pubkey must be set in order to verify
     * the received certificate.
     */
    Sv2HandshakeState(CKey&& static_key,
                    XOnlyPubKey&& authority_pubkey):
                    m_static_key{static_key},
                    m_authority_pubkey{authority_pubkey} {
                        m_our_static_ellswift_pk = static_key.EllSwiftCreate(MakeByteSpan(GetRandHash()));
                    };

    /*
     * If we are the responder, the certificate must be set
     */
    Sv2HandshakeState(CKey&& static_key,
                      Sv2SignatureNoiseMessage&& certificate):
                      m_static_key{static_key},
                      m_certificate{certificate} {
                          m_our_static_ellswift_pk = static_key.EllSwiftCreate(MakeByteSpan(GetRandHash()));
                      };

    /** Handshake step 1 for initiator: -> e */
    void WriteMsgEphemeralPK(Span<std::byte> msg);
    /** Handshake step 1 for responder: -> e */
    void ReadMsgEphemeralPK(Span<std::byte> msg);
    /** During handshake step 2, put our ephmeral key, static key
      * and certificate in the buffer: <- e, ee, s, es, SIGNATURE_NOISE_MESSAGE
      */
    void WriteMsgES(Span<std::byte> msg);
    /** During handshake step 2, read the remote ephmeral key, static key
      * and certificate. Verify their certificate.
      * <- e, ee, s, es, SIGNATURE_NOISE_MESSAGE
      */
    [[nodiscard]] bool ReadMsgES(Span<std::byte> msg);

    std::array<Sv2CipherState, 2> SplitSymmetricState();
    uint256 GetHashOutput();

private:
    /** Our static key (s) */
    CKey m_static_key;
    /** EllSwift encoded static key, for optimized ECDH */
    EllSwiftPubKey m_our_static_ellswift_pk;
    /** Our ephemeral key (e) */
    CKey m_ephemeral_key;
    /** EllSwift encoded ephemeral key, for optimized ECDH */
    EllSwiftPubKey m_our_ephemeral_ellswift_pk;
    /** Remote static key (rs) */
    EllSwiftPubKey m_remote_static_ellswift_pk;
    /** Remote ephemeral key (re) */
    EllSwiftPubKey m_remote_ephemeral_ellswift_pk;
    Sv2SymmetricState m_symmetric_state;
    /** Certificate signed by m_authority_pubkey. */
    std::optional<Sv2SignatureNoiseMessage> m_certificate;
    /** Authority public key. */
    std::optional<XOnlyPubKey> m_authority_pubkey;

    /** Generate ephemeral key, sets set m_ephemeral_key and m_our_ephemeral_ellswift_pk */
    void GenerateEphemeralKey() noexcept;
};

/**
  * Interface somewhat similar to BIP324Cipher for use by a Transport class.
  * The iniator and responder roles have their own constructor.
  * FinishHandshake() must be called after all handshake bytes have been processed.
  */
class Sv2Cipher
{
public:
    Sv2Cipher(CKey&& static_key, XOnlyPubKey&& authority_pubkey);
    Sv2Cipher(CKey&& static_key, Sv2SignatureNoiseMessage&& certificate);

    Sv2Cipher(bool initiator, std::unique_ptr<Sv2HandshakeState> handshake_state):
              m_initiator{initiator}, m_handshake_state{std::move(handshake_state)} {};

    Sv2HandshakeState& GetHandshakeState();
    /**
     * Populates m_hash, m_cs1 and m_cs2 from m_handshake_state and deletes the latter.
     */
    void FinishHandshake();

    /** Decrypts a message. May only be called after FinishHandshake() */
    bool DecryptMessage(Span<std::byte> message);
    /** Encrypts a message. May only be called after FinishHandshake() */
    void EncryptMessage(Span<const std::byte> input, Span<std::byte> output);

    /* Expected size after chunking and with MAC */
    static size_t EncryptedMessageSize(size_t msg_len);

    /* Test only */
    uint256 GetHash() const;

private:
    bool m_initiator;
    std::unique_ptr<Sv2HandshakeState> m_handshake_state;

    uint256 m_hash;
    Sv2CipherState m_cs1;
    Sv2CipherState m_cs2;
};

#endif // BITCOIN_COMMON_SV2_NOISE_H
