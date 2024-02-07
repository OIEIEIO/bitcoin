#include <addresstype.h>
#include <boost/test/unit_test.hpp>
#include <common/sv2_messages.h>
#include <node/transaction.h>
#include <node/sv2_template_provider.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/transaction_utils.h>
#include <util/sock.h>
#include <util/strencodings.h>

BOOST_FIXTURE_TEST_SUITE(sv2_template_provider_tests, TestChain100Setup)

/**
  * A class for testing the Template Provider. Each TPTester encapsulates a
  * Sv2TemplateProvider (the one being tested) as well as a Sv2Cipher
  * to act as the other side.
  */
class TPTester {
private:
    std::unique_ptr<Sv2Transport> m_peer_transport; //!< Transport for peer
    std::unique_ptr<Sock> m_peer_socket;

public:
    std::unique_ptr<Sv2TemplateProvider> m_tp; //!< Sv2TemplateProvider being tested

    TPTester(ChainstateManager& chainman, CTxMemPool& mempool)
    {
        m_tp = std::make_unique<Sv2TemplateProvider>(chainman, mempool);
    }

    bool start()
    {
        bool started = m_tp->Start(Sv2TemplateProviderOptions { .port = 18447 });
        if (! started) return false;
        return true;
    }

    void SendPeerBytes()
    {
        int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
        ssize_t sent = 0;
        const auto& [data, more] =m_peer_transport->GetBytesToSendSv2(/*have_next_message=*/false);
        BOOST_REQUIRE(data.size() > 0);
        sent = m_peer_socket->Send(data.data(), data.size(), flags);
        m_peer_transport->MarkBytesSent(sent);

        BOOST_REQUIRE(sent > 0);
    }

    // Have the peer receive and process bytes:
    void PeerReceiveBytes(size_t bytes_expected)
    {
        uint8_t bytes_received_buf[0x10000];
        const auto num_bytes_received = m_peer_socket->Recv(bytes_received_buf, sizeof(bytes_received_buf), MSG_DONTWAIT);
        BOOST_REQUIRE_EQUAL(num_bytes_received, bytes_expected);

        // Have peer process received bytes:
        Span<const uint8_t> received = Span(bytes_received_buf).subspan(0, num_bytes_received);
        BOOST_REQUIRE(m_peer_transport->ReceivedBytes(received));
    }

    /* Create a new client and perform handshake */
    void handshake()
    {
        m_peer_transport.reset();

        auto peer_static_key{GenerateRandomKey()};
        m_peer_transport = std::make_unique<Sv2Transport>(std::move(peer_static_key), m_tp->m_authority_pubkey);

        // Connect client via socket to Template Provider

        std::optional<CService> tp{Lookup("127.0.0.1", 18447, /*fAllowLookup=*/false)};
        BOOST_REQUIRE(tp);
        m_peer_socket = CreateSock(*tp);
        BOOST_REQUIRE(m_peer_socket);
        bool connected = ConnectSocketDirectly(*tp, *m_peer_socket, /*nConnectTimeout=*/100, true);
        BOOST_REQUIRE(connected);

        // Flush transport for handshake part 1
        SendPeerBytes();

        // Read handshake part 2 from transport
        constexpr auto timeout = std::chrono::milliseconds(500);
        Sock::Event occurred;
        BOOST_REQUIRE(m_peer_socket->Wait(timeout, Sock::RECV, &occurred));
        BOOST_REQUIRE(occurred != 0);

        PeerReceiveBytes(Sv2HandshakeState::HANDSHAKE_STEP2_SIZE);
    }

    void receiveMessage(Sv2NetMsg msg)
    {
        // Client encrypts message and puts it on the transport:
        BOOST_REQUIRE(m_peer_transport->SetMessageToSend(msg));
        SendPeerBytes();
    }

    void ProcessOurResponse(size_t reply_bytes_expected)
    {
        // Respond to peer
        constexpr auto timeout = std::chrono::milliseconds(500);
        Sock::Event occurred;
        BOOST_REQUIRE(m_peer_socket->Wait(timeout, Sock::RECV, &occurred));
        BOOST_REQUIRE(occurred != 0);

        // Have peer process response bytes:
        PeerReceiveBytes(reply_bytes_expected);
    }

    bool IsConnected()
    {
        return m_tp->ConnectedClients() > 0;
    }

    bool IsFullyConnected()
    {
        return m_tp->FullyConnectedClients() > 0;
    }

    Sv2NetMsg SetupConnectionMsg()
    {
        std::vector<uint8_t> bytes{
            0x02,                                                 // protocol
            0x02, 0x00,                                           // min_version
            0x02, 0x00,                                           // max_version
            0x01, 0x00, 0x00, 0x00,                               // flags
            0x07, 0x30, 0x2e, 0x30, 0x2e, 0x30, 0x2e, 0x30,       // endpoint_host
            0x61, 0x21,                                           // endpoint_port
            0x07, 0x42, 0x69, 0x74, 0x6d, 0x61, 0x69, 0x6e,       // vendor
            0x08, 0x53, 0x39, 0x69, 0x20, 0x31, 0x33, 0x2e, 0x35, // hardware_version
            0x1c, 0x62, 0x72, 0x61, 0x69, 0x69, 0x6e, 0x73, 0x2d, 0x6f, 0x73, 0x2d, 0x32, 0x30,
            0x31, 0x38, 0x2d, 0x30, 0x39, 0x2d, 0x32, 0x32, 0x2d, 0x31, 0x2d, 0x68, 0x61, 0x73,
            0x68, // firmware
            0x10, 0x73, 0x6f, 0x6d, 0x65, 0x2d, 0x64, 0x65, 0x76, 0x69, 0x63, 0x65, 0x2d, 0x75,
            0x75, 0x69, 0x64, // device_id
        };

        node::Sv2NetHeader setup_conn_header = node::Sv2NetHeader{node::Sv2MsgType::SETUP_CONNECTION, static_cast<uint32_t>(bytes.size())};
        return node::Sv2NetMsg{std::move(setup_conn_header), std::move(bytes)};
    }
};

BOOST_AUTO_TEST_CASE(client_tests)
{
    TPTester tester{*m_node.chainman, *m_node.mempool};
    BOOST_REQUIRE(tester.start());

    BOOST_REQUIRE(!tester.IsConnected());
    tester.handshake();
    BOOST_REQUIRE(tester.IsConnected());
    BOOST_REQUIRE(!tester.IsFullyConnected());

    // After the handshake the client must send a SetupConnection message to the
    // Template Provider.

    // An empty SetupConnection message should cause disconnection
    node::Sv2NetHeader setup_conn_header{node::Sv2MsgType::SETUP_CONNECTION, 0};
    node::Sv2NetMsg sv2_msg{std::move(setup_conn_header), {}};
    tester.receiveMessage(sv2_msg);
    tester.ProcessOurResponse(0);

    BOOST_REQUIRE(!tester.IsConnected());

    // Reconnect
    tester.handshake();

    node::Sv2NetMsg setup{tester.SetupConnectionMsg()};
    // SetupConnection.Success is 6 bytes
    tester.receiveMessage(setup);
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + 6 + Poly1305::TAGLEN);
    BOOST_REQUIRE(tester.IsFullyConnected());

    // There should be no block templates before any client gave us their coinbase
    // output data size:
    BOOST_REQUIRE(tester.m_tp->GetBlockTemplates().empty());

    std::vector<uint8_t> coinbase_output_max_additional_size_bytes{
        0x01, 0x00, 0x00, 0x00
    };
    node::Sv2NetHeader cb_header{node::Sv2MsgType::COINBASE_OUTPUT_DATA_SIZE, 4};
    node::Sv2NetMsg msg{std::move(cb_header), std::move(coinbase_output_max_additional_size_bytes)};
    // The reply should be NewTemplate and SetNewPrevHash, sent seperately
    tester.receiveMessage(msg);
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + 91 + Poly1305::TAGLEN);
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + 80 + Poly1305::TAGLEN);

    // There should now be one template
    BOOST_REQUIRE_EQUAL(tester.m_tp->GetBlockTemplates().size(), 1);

    // Move mock time by at least DEFAULT_SV2_INTERVAL
    // If the mempool doesn't change, no new template is generated.
    SetMockTime(GetMockTime() + std::chrono::seconds{DEFAULT_SV2_INTERVAL});
    // Briefly wait for the timer in ThreadSv2Handler and block creation
    UninterruptibleSleep(std::chrono::milliseconds{200});
    BOOST_REQUIRE_EQUAL(tester.m_tp->GetBlockTemplates().size(), 1);

    // Create a transaction with a large fee
    // Don't hold on to it
    size_t tx_size;
    CKey key = GenerateRandomKey();
    CScript locking_script = GetScriptForDestination(PKHash(key.GetPubKey()));
    {

        auto mtx = CreateValidMempoolTransaction(/*input_transaction=*/m_coinbase_txns[0], /*input_vout=*/0,
                                                        /*input_height=*/0, /*input_signing_key=*/coinbaseKey,
                                                        /*output_destination=*/locking_script,
                                                        /*output_amount=*/CAmount(49 * COIN), /*submit=*/true);
        CTransactionRef tx = MakeTransactionRef(mtx);

        // Get serialized transaction size
        DataStream ss;
        ss << TX_WITH_WITNESS(tx);
        tx_size = ss.size();
    }

    // Move mock time by at least DEFAULT_SV2_INTERVAL
    SetMockTime(GetMockTime() + std::chrono::seconds{DEFAULT_SV2_INTERVAL});
    // Briefly wait for the timer in ThreadSv2Handler and block creation
    UninterruptibleSleep(std::chrono::milliseconds{200});

    // Check that there's a new template
    BOOST_REQUIRE_EQUAL(tester.m_tp->GetBlockTemplates().size(), 2);

    // Expect our peer te receive a NewTemplate message
    // This time it should contain the 32 byte prevhash (unchanged)
    tester.PeerReceiveBytes(SV2_HEADER_ENCRYPTED_SIZE + 91 + 32 + Poly1305::TAGLEN);

    // Have the peer send us RequestTransactionData
    // We should reply with RequestTransactionData.Success
    node::Sv2NetHeader req_tx_data_header{node::Sv2MsgType::REQUEST_TRANSACTION_DATA, 8};
    std::vector<uint8_t> template_id_bytes {
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    msg = node::Sv2NetMsg{std::move(req_tx_data_header), std::move(template_id_bytes)};
    tester.receiveMessage(msg);
    const size_t template_id_size = 8;
    const size_t excess_data_size = 2 + 32;
    size_t tx_list_size = 2 + 3 + tx_size;
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + template_id_size + excess_data_size + tx_list_size + Poly1305::TAGLEN);

    // RBF the transaction with with > DEFAULT_SV2_FEE_DELTA
    CreateValidMempoolTransaction(/*input_transaction=*/m_coinbase_txns[0], /*input_vout=*/0,
                                                    /*input_height=*/0, /*input_signing_key=*/coinbaseKey,
                                                    /*output_destination=*/locking_script,
                                                    /*output_amount=*/CAmount(48 * COIN), /*submit=*/true);

    // Move mock time by at least DEFAULT_SV2_INTERVAL
    SetMockTime(GetMockTime() + std::chrono::seconds{DEFAULT_SV2_INTERVAL});
    // Briefly wait for the timer in ThreadSv2Handler and block creation
    UninterruptibleSleep(std::chrono::milliseconds{200});

    // Check that there's a new template
    BOOST_REQUIRE_EQUAL(tester.m_tp->GetBlockTemplates().size(), 3);

    // Expect our peer te receive a NewTemplate message
    tester.PeerReceiveBytes(SV2_HEADER_ENCRYPTED_SIZE + 91 + 32 + Poly1305::TAGLEN);

    // Have the peer send us RequestTransactionData for the old template
    // We should reply with RequestTransactionData.Success, and the original
    // (replaced) transaction
    tester.receiveMessage(msg);
    tx_list_size = 2 + 3 + tx_size;
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + template_id_size + excess_data_size + tx_list_size + Poly1305::TAGLEN);

    // Create a new block
    mineBlocks(1);

    // We should send out another NewTemplate and SetNewPrevHash
    // The new template contains the new prevhash.
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + 91 + 32 + Poly1305::TAGLEN);
    // The SetNewPrevHash message is redundant
    // TODO: don't send it?
    // Background: in the future we want to send an empty or optimistic template
    //             before a block is found, so ASIC's can preload it. We would
    //             then immedidately send a SetNewPrevHash message when there's
    //             a new block, and contruct a better template _after_ that.
    tester.ProcessOurResponse(SV2_HEADER_ENCRYPTED_SIZE + 80 + Poly1305::TAGLEN);

    // Templates are briefly preserved
    BOOST_REQUIRE_EQUAL(tester.m_tp->GetBlockTemplates().size(), 4);

    // Do not provide transactions for stale templates
    // TODO

    // But do allow SubmitSolution
    // TODO

    // Until after some time
    SetMockTime(GetMockTime() + std::chrono::seconds{15});
    UninterruptibleSleep(std::chrono::milliseconds{200});
    BOOST_REQUIRE_EQUAL(tester.m_tp->GetBlockTemplates().size(), 1);

}

BOOST_AUTO_TEST_SUITE_END()