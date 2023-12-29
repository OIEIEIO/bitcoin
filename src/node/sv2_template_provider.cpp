#include <node/sv2_template_provider.h>

#include <common/args.h>
#include <common/sv2_transport.h>
#include <consensus/merkle.h>
#include <txmempool.h>
#include <util/thread.h>
#include <validation.h>

bool Sv2TemplateProvider::Start(const Sv2TemplateProviderOptions& options)
{
    Init(options);

    // Here we are checking if we can bind to the port. If we can't, then exit
    // early and shutdown the node gracefully. This would be called in init.cpp
    // and allows the caller to see that the node is unable to run with the current
    // sv2 config.
    //
    // The socket is dropped within this scope and re-opened on the same port in
    // ThreadSv2Handler() when the node has finished IBD.
    try {
        auto sock = BindListenPort(options.port);
    } catch (const std::runtime_error& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Template Provider failed to bind to port %d: %s\n", options.port, e.what());
        return false;
    }

    m_thread_sv2_handler = std::thread(&util::TraceThread, "sv2", [this] { ThreadSv2Handler(); });
    return true;
}

void Sv2TemplateProvider::Init(const Sv2TemplateProviderOptions& options)
{
    m_minimum_fee_delta = gArgs.GetIntArg("-sv2feedelta", DEFAULT_SV2_FEE_DELTA);
    m_port = options.port;
    m_protocol_version = options.protocol_version;
    m_optional_features = options.optional_features;
    m_default_coinbase_tx_additional_output_size = options.default_coinbase_tx_additional_output_size;
    m_default_future_templates = options.default_future_templates;
}

Sv2TemplateProvider::~Sv2TemplateProvider()
{
    for (const auto& client : m_sv2_clients) {
        client->m_disconnect_flag = true;
    }

    DisconnectFlagged();
    Interrupt();
    StopThreads();
}

void Sv2TemplateProvider::Interrupt()
{
    m_flag_interrupt_sv2 = true;
}

void Sv2TemplateProvider::StopThreads()
{
    if (m_thread_sv2_handler.joinable()) {
        m_thread_sv2_handler.join();
    }
}

std::shared_ptr<Sock> Sv2TemplateProvider::BindListenPort(uint16_t port) const
{
    const CService addr_bind = LookupNumeric("0.0.0.0", port);

    auto sock = CreateSock(addr_bind);
    if (!sock) {
        throw std::runtime_error("Sv2 Template Provider cannot create socket");
    }

    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);

    if (!addr_bind.GetSockAddr(reinterpret_cast<struct sockaddr*>(&sockaddr), &len)) {
        throw std::runtime_error("Sv2 Template Provider failed to get socket address");
    }

    if (sock->Bind(reinterpret_cast<struct sockaddr*>(&sockaddr), len) == SOCKET_ERROR) {
        const int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE) {
            throw std::runtime_error(strprintf("Unable to bind to %d on this computer. %s is probably already running.\n", port, PACKAGE_NAME));
        }

        throw std::runtime_error(strprintf("Unable to bind to %d on this computer (bind returned error %s )\n", port, NetworkErrorString(nErr)));
    }

    constexpr int max_pending_conns{4096};
    if (sock->Listen(max_pending_conns) == SOCKET_ERROR) {
        throw std::runtime_error("Sv2 Template Provider listening socket has an error listening");
    }

    return sock;
}
class Timer {
private:
    std::chrono::seconds m_interval;
    std::chrono::steady_clock::time_point m_last_triggered;

public:
    Timer() {
        m_interval = std::chrono::seconds(gArgs.GetIntArg("-sv2interval", DEFAULT_SV2_INTERVAL));
        // Initialize the timer to a time point far in the past
        m_last_triggered = std::chrono::steady_clock::now() - std::chrono::hours(1);
    }

    bool trigger() {
        auto now = std::chrono::steady_clock::now();
        if (now - m_last_triggered >= m_interval) {
            m_last_triggered = now;
            return true;
        }
        return false;
    }
};

void Sv2TemplateProvider::DisconnectFlagged()
{
    // Remove clients that are flagged for disconnection.
    m_sv2_clients.erase(
        std::remove_if(m_sv2_clients.begin(), m_sv2_clients.end(), [](const auto &client) {
            return client->m_disconnect_flag;
    }), m_sv2_clients.end());
}

void Sv2TemplateProvider::ThreadSv2Handler()
{
    Timer timer;
    unsigned int mempool_last_update = 0;
    unsigned int template_last_update = 0;

    while (!m_flag_interrupt_sv2) {
        if (m_chainman.IsInitialBlockDownload()) {
            m_interrupt_sv2.sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // If we've left IBD. Create the listening socket for new sv2 connections.
        if (!m_listening_socket) {
            try {
                auto socket = BindListenPort(m_port);
                m_listening_socket = std::move(socket);
            } catch (const std::runtime_error& e) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "thread shutting down due to exception: %s\n", e.what());
                Interrupt();
                continue;
            }

            LogPrintLevel(BCLog::SV2, BCLog::Level::Info, "Template Provider listening on port: %d\n", m_port);
        }

        DisconnectFlagged();

        bool best_block_changed = [this]() {
            WAIT_LOCK(g_best_block_mutex, lock);
            auto checktime = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            g_best_block_cv.wait_until(lock, checktime);
            if (m_best_prev_hash.m_prev_hash != g_best_block) {
                m_best_prev_hash.m_prev_hash = g_best_block;
                return true;
            }
            return false;
        }();


        /** TODO: only look for mempool updates that (likely) impact the next block.
         *        See `doc/stratum-v2.md#mempool-monitoring`
         */
        mempool_last_update = m_mempool.GetTransactionsUpdated();
        bool should_make_template = false;

        if (best_block_changed) {
            // Clear the block cache when the best known block changes since all
            // previous work is now invalid.
            BlockCache block_cache;
            m_block_cache.swap(block_cache);

            for (auto& client : m_sv2_clients) {
                client->m_latest_submitted_template_fees = 0;
            }

            // Build a new best template, best prev hash and update the block cache.
            should_make_template = true;
            template_last_update = mempool_last_update;
        } else if (timer.trigger() && mempool_last_update > template_last_update) {
            should_make_template = true;
        }

        if (should_make_template) {
            // Update all clients with the new template and prev hash.
            for (const auto& client : m_sv2_clients) {
                // For newly connected clients, we call SendWork after receiving
                // CoinbaseOutputDataSize.
                if (client->m_coinbase_tx_outputs_size == 0) continue;
                if (!SendWork(*client.get(), /*send_new_prevhash=*/best_block_changed)) {
                    client->m_disconnect_flag = true;
                    continue;
                }
            }
        }

        // Poll/Select the sockets that need handling.
        Sock::EventsPerSock events_per_sock = GenerateWaitSockets(m_listening_socket, m_sv2_clients);

        constexpr auto timeout = std::chrono::milliseconds(50);
        if (!events_per_sock.begin()->first->WaitMany(timeout, events_per_sock)) {
            continue;
        }

        // Accept any new connections for sv2 clients.
        const auto listening_sock = events_per_sock.find(m_listening_socket);
        if (listening_sock != events_per_sock.end() && listening_sock->second.occurred & Sock::RECV) {
            struct sockaddr_storage sockaddr;
            socklen_t sockaddr_len = sizeof(sockaddr);

            auto sock = m_listening_socket->Accept(reinterpret_cast<struct sockaddr*>(&sockaddr), &sockaddr_len);
            if (sock) {
                m_sv2_clients.emplace_back(std::make_unique<Sv2Client>(Sv2Client{std::move(sock)}));
            }
        }

        // Process messages from connected sv2_clients.
        for (auto& client : m_sv2_clients) {
            bool has_received_data = false;
            bool has_error_occurred = false;

            const auto it = events_per_sock.find(client->m_sock);
            if (it != events_per_sock.end()) {
                has_received_data = it->second.occurred & Sock::RECV;
                has_error_occurred = it->second.occurred & Sock::ERR;
            }

            if (has_error_occurred) {
                client->m_disconnect_flag = true;
            }

            if (has_received_data) {
                uint8_t bytes_received_buf[0x10000];

                const auto num_bytes_received = client->m_sock->Recv(bytes_received_buf, sizeof(bytes_received_buf), MSG_DONTWAIT);
                LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Num bytes received: %d\n", num_bytes_received);

                if (num_bytes_received <= 0) {
                    client->m_disconnect_flag = true;
                    continue;
                }

                try
                {
                    auto msg_ = Span(bytes_received_buf, num_bytes_received);
                    Span<std::byte> msg(reinterpret_cast<std::byte*>(msg_.data()), msg_.size());

                    if (!client->m_noise->HandshakeComplete()) {
                        ProcessMaybeSv2Handshake(*client.get(), msg);
                    } else {
                        auto sv2_msgs = ReadAndDecryptSv2NetMsgs(*client.get(), msg);

                        for (auto& m : sv2_msgs)
                        {
                            ProcessSv2Message(m, *client.get());
                        }
                    }
                } catch (const std::exception& e) {
                    LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received error when processing client message: %s\n", e.what());
                    client->m_disconnect_flag = true;
                }
            }
        }
    }
}

void Sv2TemplateProvider::ProcessMaybeSv2Handshake(Sv2Client& client, Span<std::byte> buffer)
{
    const SessionState state_before = client.m_noise->GetSessionState();
    Assume(state_before != SessionState::TRANSPORT);

    bool res = client.m_noise->ProcessMaybeHandshake(buffer, /*send=*/false);
    if (!res) throw std::runtime_error("Failed to parse Msg E from client\n");

    // TODO: consider modifying ReadMsg to optionally return a reply, so
    //       we don't need to access client.m_noise->GetSessionState()
    if (state_before == SessionState::HANDSHAKE_STEP_1) {
        // Expect to have read the E msg.
        // Expect state transition to have happened
        Assume(client.m_noise->GetSessionState() == SessionState::HANDSHAKE_STEP_2);

        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Send noise handshake reply: ES\n");
        std::byte msg_es[INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH];
        Span<std::byte> msg_es_span(msg_es, INITIATOR_EXPECTED_HANDSHAKE_MESSAGE_LENGTH);
        res  = client.m_noise->ProcessMaybeHandshake(msg_es_span, /*send=*/true);
        if(!res) throw std::runtime_error("Failed to construct Msg ES\n");
        if (!SendBuf(client, msg_es_span)) {
            throw std::runtime_error("Sv2TemplateProvider::ProcessSv2Message(): Failed to send Msg ES to client\n");
        }
    }
}

Sv2TemplateProvider::NewWorkSet Sv2TemplateProvider::BuildNewWorkSet(bool future_template, unsigned int coinbase_output_max_additional_size)
{
    node::BlockAssembler::Options options;

    // Reducing the size of nBlockMaxWeight by the coinbase output additional
    // size allows the miner extra weighted bytes in their coinbase space.
    Assume(coinbase_output_max_additional_size <= MAX_BLOCK_WEIGHT);
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT - coinbase_output_max_additional_size;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    const auto time_start{SteadyClock::now()};
    auto blocktemplate = node::BlockAssembler(m_chainman.ActiveChainstate(), &m_mempool, options).CreateNewBlock(CScript());
    LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Assemble template: %.2fms\n",
        Ticks<MillisecondsDouble>(SteadyClock::now() - time_start));
    node::Sv2NewTemplateMsg new_template{blocktemplate->block, m_template_id, future_template};
    node::Sv2SetNewPrevHashMsg set_new_prev_hash{blocktemplate->block, m_template_id};

    return NewWorkSet { new_template, std::move(blocktemplate), set_new_prev_hash};
}

bool Sv2TemplateProvider::SendWork(Sv2Client& client, bool send_new_prevhash)
{
    // The current implementation doesn't create templates for future empty
    // or speculative blocks. Despite that, we first send NewTemplate with
    // future_template set to true, followed by SetNewPrevHash. We do this
    // both when first connecting and when a new block is found.
    //
    // When the template is update to take newer mempool transactions into
    // account, we set future_template to false and don't send SetNewPrevHash.

    // TODO: reuse template_id for clients with the same m_default_coinbase_tx_additional_output_size
    ++m_template_id;
    auto new_work_set = BuildNewWorkSet(/*future_template=*/send_new_prevhash, client.m_coinbase_tx_outputs_size);

    // Do not submit new template if the fee increase is insufficient:
    CAmount fees = 0;
    for (CAmount fee : new_work_set.block_template->vTxFees) {
        // Skip coinbase
        if (fee < 0) continue;
        fees += fee;
    }
    if (!send_new_prevhash && client.m_latest_submitted_template_fees + m_minimum_fee_delta > fees) return true;

    m_block_cache.insert({m_template_id, std::move(new_work_set.block_template)});

    auto msg = node::Sv2NetMsg{new_work_set.new_template};
    if (!EncryptAndSendMessage(client, msg)) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error sending NewTemplate message\n");
        return false;
    }

    if (send_new_prevhash) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x20 SetNewPrevHash\n");
        auto msg = node::Sv2NetMsg{new_work_set.prev_hash};

        if (!EncryptAndSendMessage(client, msg)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error sending SetNewPrevHash message\n");
            return false;
        }
    }

    client.m_latest_submitted_template_fees = fees;

    return true;
}

Sock::EventsPerSock Sv2TemplateProvider::GenerateWaitSockets(const std::shared_ptr<Sock>& listen_socket, const Clients& sv2_clients) const
{
    Sock::EventsPerSock events_per_sock;
    events_per_sock.emplace(listen_socket, Sock::Events(Sock::RECV));

    for (const auto& client : sv2_clients) {
        if (!client->m_disconnect_flag && client->m_sock) {
            events_per_sock.emplace(client->m_sock, Sock::Events{Sock::RECV | Sock::ERR});
        }
    }

    return events_per_sock;
}

void Sv2TemplateProvider::ProcessSv2Message(const node::Sv2NetMsg& sv2_net_msg, Sv2Client& client)
{
    DataStream ss (sv2_net_msg.m_msg);

    switch (sv2_net_msg.m_sv2_header.m_msg_type)
    {
    case node::Sv2MsgType::SETUP_CONNECTION:
    {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x00 SetupConnection\n");

        if (client.m_setup_connection_confirmed) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Client connection has already been confirmed\n");
            return;
        }

        node::Sv2SetupConnectionMsg setup_conn;
        try {
            ss >> setup_conn;
        } catch (const std::exception& e) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received invalid SetupConnection message: %s\n", e.what());
            client.m_disconnect_flag = true;
            return;
        }

        // Disconnect a client that connects on the wrong subprotocol.
        if (setup_conn.m_protocol != TP_SUBPROTOCOL) {
            node::Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"unsupported-protocol"}};
            auto msg = node::Sv2NetMsg{setup_conn_err};

            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x02 SetupConnectionError\n");
            if (!EncryptAndSendMessage(client, msg)) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Failed to send Sv2SetupConnectionError message\n");
            }

            client.m_disconnect_flag = true;
            return;
        }

        // Disconnect a client if they are not running a compatible protocol version.
        if ((m_protocol_version < setup_conn.m_min_version) || (m_protocol_version > setup_conn.m_max_version)) {
            node::Sv2SetupConnectionErrorMsg setup_conn_err{setup_conn.m_flags, std::string{"protocol-version-mismatch"}};
            auto msg = node::Sv2NetMsg{setup_conn_err};
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x02 SetupConnection.Error\n");

            if (!EncryptAndSendMessage(client, msg)) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Failed to send SetupConnection.Error message\n");
            }

            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received a connection with incompatible protocol_versions: min_version: %d, max_version: %d\n", setup_conn.m_min_version, setup_conn.m_max_version);
            client.m_disconnect_flag = true;
            return;
        }

        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x01 SetupConnection.Success\n");
        node::Sv2SetupConnectionSuccessMsg setup_success{m_protocol_version, m_optional_features};
        auto msg = node::Sv2NetMsg{setup_success};

        if (!EncryptAndSendMessage(client, msg)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Failed to send SetupConnection.Success message\n");
            client.m_disconnect_flag = true;
            return;
        }

        client.m_setup_connection_confirmed = true;

        break;
    }
    case node::Sv2MsgType::COINBASE_OUTPUT_DATA_SIZE:
    {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x70 CoinbaseOutputDataSize\n");

        if (!client.m_setup_connection_confirmed) {
            client.m_disconnect_flag = true;
            return;
        }

        node::Sv2CoinbaseOutputDataSizeMsg coinbase_output_data_size;
        try {
            ss >> coinbase_output_data_size;
            client.m_coinbase_output_data_size_recv = true;
        } catch (const std::exception& e) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received invalid CoinbaseOutputDataSize message: %s\n", e.what());
            client.m_disconnect_flag = true;
            return;
        }

        uint32_t max_additional_size = coinbase_output_data_size.m_coinbase_output_max_additional_size;
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "coinbase_output_max_additional_size=%d bytes\n", max_additional_size);

        if (max_additional_size > MAX_BLOCK_WEIGHT) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received impossible CoinbaseOutputDataSize: %d\n", max_additional_size);
            client.m_disconnect_flag = true;
            return;
        }

        client.m_coinbase_tx_outputs_size = coinbase_output_data_size.m_coinbase_output_max_additional_size;

        // Send new template and prevout
        if (!SendWork(client, /*send_new_prevhash=*/true)) {
            return;
        }

        break;
    }
    case node::Sv2MsgType::SUBMIT_SOLUTION: {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x60 SubmitSolution\n");

        if (!client.m_setup_connection_confirmed && !client.m_coinbase_output_data_size_recv) {
            client.m_disconnect_flag = true;
            return;
        }

        node::Sv2SubmitSolutionMsg submit_solution;
        try {
            ss >> submit_solution;
        } catch (const std::exception& e) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received invalid SubmitSolution message: %e\n", e.what());
            return;
        }

        auto cached_block = m_block_cache.find(submit_solution.m_template_id);
        if (cached_block != m_block_cache.end()) {
            CBlock& block = (*cached_block->second).block;

            auto coinbase_tx = CTransaction(std::move(submit_solution.m_coinbase_tx));
            auto cb = MakeTransactionRef(std::move(coinbase_tx));

            if (block.vtx.size() == 0) {
                block.vtx.push_back(cb);
            } else {
                block.vtx[0] = cb;
            }

            block.nVersion = submit_solution.m_version;
            block.nTime = submit_solution.m_header_timestamp;
            block.nNonce = submit_solution.m_header_nonce;
            block.hashMerkleRoot = BlockMerkleRoot(block);

            auto blockptr = std::make_shared<CBlock>(std::move(block));
            bool new_block{true};

            m_chainman.ProcessNewBlock(blockptr, true /* force_processing */, true /* min_pow_checked */, &new_block);
        }

        break;
    }

    case node::Sv2MsgType::REQUEST_TRANSACTION_DATA:
    {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Received 0x73 RequestTransactionData\n");

        node::Sv2RequestTransactionDataMsg request_tx_data;

        try {
            ss >> request_tx_data;
        } catch (const std::exception& e) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Received invalid RequestTransactionData message: %e\n", e.what());
            return;
        }

        auto cached_block = m_block_cache.find(request_tx_data.m_template_id);
        if (cached_block != m_block_cache.end()) {
            CBlock& block = (*cached_block->second).block;

            std::vector<uint8_t> witness_reserve_value;
            if (!block.IsNull()) {
                auto scriptWitness = block.vtx[0]->vin[0].scriptWitness;
                if (!scriptWitness.IsNull()) {
                    std::copy(scriptWitness.stack[0].begin(), scriptWitness.stack[0].end(), std::back_inserter(witness_reserve_value));
                }
            }
std::vector<CTransactionRef> txs;
            if (block.vtx.size() > 0) {
                std::copy(block.vtx.begin() + 1, block.vtx.end(), std::back_inserter(txs));
            }

            node::Sv2RequestTransactionDataSuccessMsg request_tx_data_success{request_tx_data.m_template_id, std::move(witness_reserve_value), std::move(txs)};

            auto msg = node::Sv2NetMsg{request_tx_data_success};
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x74 RequestTransactionData.Success\n");

            if (!EncryptAndSendMessage(client, msg)) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error sending RequestTransactionData.Success message\n");
                client.m_disconnect_flag = true;
                return;
            }
        } else {
            node::Sv2RequestTransactionDataErrorMsg request_tx_data_error{request_tx_data.m_template_id, "template-id-not-found"};

            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Send 0x75 RequestTransactionData.Error\n");
            auto msg = node::Sv2NetMsg{request_tx_data_error};

            if (!EncryptAndSendMessage(client, msg)) {
                LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error sending RequestTransactionData.Error message\n");
                client.m_disconnect_flag = true;
                return;
            }
        }

        break;
    }

    default: {
        uint8_t msg_type[1]{uint8_t(sv2_net_msg.m_sv2_header.m_msg_type)};
        LogPrintLevel(BCLog::SV2, BCLog::Level::Warning, "Received unknown message type 0x%s\n", HexStr(msg_type));
        break;
    }
    }
}

bool Sv2TemplateProvider::EncryptAndSendMessage(Sv2Client& client, node::Sv2NetMsg& net_msg)
{
    const size_t encrypted_msg_size = Sv2NoiseSession::EncryptedMessageSize(net_msg.m_msg.size());
    std::vector<std::byte> buffer(SV2_HEADER_ENCRYPTED_SIZE + encrypted_msg_size, std::byte(0));
    Span<std::byte> buffer_span{MakeWritableByteSpan(buffer)};

    // Header
    DataStream ss_header_plain{};
    ss_header_plain << net_msg.m_sv2_header;
    LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Header: %s\n", HexStr(ss_header_plain));
    Span<std::byte> header_encrypted{buffer_span.subspan(0, SV2_HEADER_ENCRYPTED_SIZE)};
    client.m_noise->EncryptMessage(ss_header_plain, header_encrypted);

    // Payload
    Span<std::byte> payload_plain = MakeWritableByteSpan(net_msg.m_msg);
    // TODO: truncate very long messages, about 100 bytes at the start and end
    //       is probably enough for most debugging.
    // LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Payload: %s\n", HexStr(payload_plain));
    Span<std::byte> payload_encrypted{buffer_span.subspan(SV2_HEADER_ENCRYPTED_SIZE, encrypted_msg_size)};
    client.m_noise->EncryptMessage(payload_plain, payload_encrypted);

    return SendBuf(client, buffer_span);
};

std::vector<node::Sv2NetMsg> Sv2TemplateProvider::ReadAndDecryptSv2NetMsgs(Sv2Client& client, Span<std::byte> buffer)
{
    Assume(client.m_noise->GetSessionState() == SessionState::TRANSPORT);

    size_t bytes_read = 0;
    std::vector<node::Sv2NetMsg> sv2_msgs;

    while (bytes_read < buffer.size())
    {
        // Decrypt the header.
        Span<std::byte> encrypted_header = Span(&buffer[bytes_read], SV2_HEADER_ENCRYPTED_SIZE);
        if  (!client.m_noise->DecryptMessage(encrypted_header)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Failed to decrypt header\n");
            client.m_disconnect_flag = true;
            break;
        }
        bytes_read += SV2_HEADER_ENCRYPTED_SIZE;

        Span<std::byte> decrypted_header = encrypted_header.subspan(0, SV2_HEADER_PLAIN_SIZE);
        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Header: %s\n", HexStr(decrypted_header));

        // Decode header
        DataStream ss_header{decrypted_header};
        node::Sv2NetHeader header;
        ss_header >> header;

        // Decrypt the payload
        size_t expanded_size = Sv2NoiseSession::EncryptedMessageSize(header.m_msg_len);
        Span<std::byte> encrypted_payload = Span(&buffer[bytes_read], expanded_size);
        if (!client.m_noise->DecryptMessage(encrypted_payload)) {
            LogPrintLevel(BCLog::SV2, BCLog::Level::Debug, "Failed to decrypt message payload\n");
            client.m_disconnect_flag = true;
            break;
        }
        Span<std::byte> payload = encrypted_payload.subspan(0, header.m_msg_len);
        bytes_read += expanded_size;

        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Payload: %s\n", HexStr(payload));

        // Add to sv2 message queue
        std::vector<uint8_t> msg_payload(payload.size());
        std::transform(payload.begin(), payload.end(), msg_payload.begin(),
                           [](std::byte b) { return static_cast<uint8_t>(b); });

        sv2_msgs.emplace_back(std::move(header), std::move(msg_payload));
    }

    return sv2_msgs;
}

bool Sv2TemplateProvider::SendBuf(const Sv2Client& client, Span<std::byte> buffer) {
    size_t total_sent = 0;
    try {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Send %d bytes\n", buffer.size());
        while (total_sent < buffer.size()) {
            ssize_t sent = client.m_sock->Send(buffer.data() + total_sent, buffer.size() - total_sent, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent > 0) {
                total_sent += sent;
            } else if (sent == 0) {
                UninterruptibleSleep(std::chrono::milliseconds{10});
            } else {
                UninterruptibleSleep(std::chrono::milliseconds{100});
            }
            LogPrintLevel(BCLog::SV2, BCLog::Level::Trace, "Sent %d bytes\n", total_sent);
        }
    } catch (const std::exception& e) {
        LogPrintLevel(BCLog::SV2, BCLog::Level::Error, "Error sending Sv2NetMsg: %s\n", e.what());
        return false;
    }

    if (total_sent != buffer.size()) {
        return false;
    }

    return true;
}
