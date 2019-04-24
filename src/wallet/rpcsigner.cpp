// Copyright (c) 2018-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>
#include <core_io.h>
#include <key_io.h>
#include <node/transaction.h>
#include <psbt.h>
#include <rpc/rawtransaction_util.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <util/strencodings.h>
#include <validation.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/psbtwallet.h>
#include <wallet/rpcdump.h>
#include <wallet/rpcsigner.h>
#include <wallet/rpcwallet.h>

#ifdef HAVE_BOOST_PROCESS

UniValue getsigners(CWallet *pwallet) {
    const std::string command = gArgs.GetArg("-signer", DEFAULT_EXTERNAL_SIGNER);
    if (command == "") throw JSONRPCError(RPC_WALLET_ERROR, "Error: restart bitcoind with -signer=<cmd>");
    std::string chain = gArgs.GetChainName();
    const bool mainnet = chain == CBaseChainParams::MAIN;
    UniValue signers;
    try {
        return ExternalSigner::Enumerate(command, pwallet->m_external_signers, mainnet);
    } catch (const ExternalSignerException& e) {
        throw JSONRPCError(RPC_WALLET_ERROR, e.what());
    }
}

static UniValue enumeratesigners(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            RPCHelpMan{"enumeratesigners\n",
                "Returns a list of external signers from -signer and associates them\n"
                "with the wallet until you stop bitcoind.\n",
                {},
                RPCResult{
                    "{\n"
                    "  \"signers\" : [                              (json array of objects)\n"
                    "    {\n"
                    "      \"masterkeyfingerprint\" : \"fingerprint\" (string) Master key fingerprint\n"
                    "    }\n"
                    "    ,...\n"
                    "  ]\n"
                    "}\n"
                },
                RPCExamples{""}
            }.ToString()
        );
    }

    UniValue signers = getsigners(pwallet);
    UniValue result(UniValue::VOBJ);
    result.pushKV("signers", signers);
    return result;
}

ExternalSigner *GetSignerForJSONRPCRequest(const JSONRPCRequest& request, int index, CWallet* pwallet) {
    if (pwallet->m_external_signers.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "First call enumeratesigners");
    }

    // If no fingerprint is specified, return the only available signer
    if (request.params.size() < size_t(index + 1) || request.params[index].isNull()) {
        if (pwallet->m_external_signers.size() > 1) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Multiple signers found, please specify which to use");
        }
        return &pwallet->m_external_signers.front();
    }

    const std::string fingerprint = request.params[index].get_str();
    for (ExternalSigner &candidate : pwallet->m_external_signers) {
        if (candidate.m_fingerprint == fingerprint) return &candidate;
    }
    throw JSONRPCError(RPC_WALLET_ERROR, "Signer fingerprint not found");
}

UniValue signerbumpfee(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3) {
        throw std::runtime_error(
            RPCHelpMan{"signerbumpfee",
                "\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n"
                "See bumpfee documentation for more details.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The txid to be bumped"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"confTarget", RPCArg::Type::NUM, /* default */ "fallback to wallet's default", "Confirmation target (in blocks)"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "true", "Whether the new transaction should still be\n"
            "                         marked bip-125 replaceable."},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
            "         \"UNSET\"\n"
            "         \"ECONOMICAL\"\n"
            "         \"CONSERVATIVE\""},
                        },
                        "options"},
                    {"fingerprint", RPCArg::Type::STR, /* default_val */ "", "master key fingerprint of signer"},
                },
                RPCResult{
            "{\n"
            "  \"txid\":    \"value\",   (string)  The id of the new transaction\n"
            "  \"origfee\":  n,         (numeric) Fee of the replaced transaction\n"
            "  \"fee\":      n,         (numeric) Fee of the new transaction\n"
            "  \"errors\":  [ str... ] (json array of strings) Errors encountered during processing (may be empty)\n"
            "}\n"
                },
                RPCExamples{
            "\nBump the fee, get the new transaction\'s txid\n" +
                    HelpExampleCli("signerbumpfee", "<txid>")
                },
            }.ToString());
    }

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    uint256 hash(ParseHashV(request.params[0], "txid"));

    CCoinControl coin_control;
    coin_control.fAllowWatchOnly = true;
    coin_control.m_signal_bip125_rbf = true;
    if (!request.params[1].isNull()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"feeRate", UniValueType(UniValue::VNUM)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);

        if (options.exists("confTarget") && options.exists("feeRate")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget and feeRate options should not both be set. Please provide either a confirmation target for fee estimation or an explicit fee rate for the transaction.");
        } else if (options.exists("confTarget")) { // TODO: alias this to conf_target
            coin_control.m_confirm_target = ParseConfirmTarget(options["confTarget"], pwallet->chain().estimateMaxBlocks());
        } else if (options.exists("feeRate")) {
            coin_control.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
            coin_control.fOverrideFeeRate = true;
        }

        if (options.exists("replaceable")) {
            coin_control.m_signal_bip125_rbf = options["replaceable"].get_bool();
        }
        if (options.exists("estimate_mode")) {
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coin_control.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);


    std::vector<std::string> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    feebumper::Result res;
    res = feebumper::CreateRateBumpTransaction(pwallet, hash, coin_control, errors, old_fee, new_fee, mtx);
    if (res != feebumper::Result::OK) {
        switch(res) {
            case feebumper::Result::INVALID_ADDRESS_OR_KEY:
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errors[0]);
                break;
            case feebumper::Result::INVALID_REQUEST:
                throw JSONRPCError(RPC_INVALID_REQUEST, errors[0]);
                break;
            case feebumper::Result::INVALID_PARAMETER:
                throw JSONRPCError(RPC_INVALID_PARAMETER, errors[0]);
                break;
            case feebumper::Result::WALLET_ERROR:
                throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
                break;
            default:
                throw JSONRPCError(RPC_MISC_ERROR, errors[0]);
                break;
        }
    }

    // Make a blank psbt
    PartiallySignedTransaction psbtx(mtx);

    // Fill transaction with out data but don't sign
    bool complete_dummy;

    TransactionError fill_psbt_error = FillPSBT(pwallet, psbtx, complete_dummy, 1, false, true);
    if (fill_psbt_error != TransactionError::OK) {
        throw JSONRPCTransactionError(fill_psbt_error);
    }

    // TODO: if more than one signer is known and no fingerprint argument is present,
    //       loop through inputs to find a matching fingerprint.
    ExternalSigner *signer = GetSignerForJSONRPCRequest(request, 2, pwallet);

    // Send to signer and process result
    std::string error;
    if( !signer->signTransaction(psbtx, error)) throw JSONRPCError(RPC_WALLET_ERROR, error);

    CMutableTransaction mtx_out;
    bool complete = FinalizeAndExtractPSBT(psbtx, mtx_out);

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);

    UniValue result(UniValue::VOBJ);

    if (complete) {
        // CTransactionRef tx(MakeTransactionRef(std::move(mtx_out)));
        // commit the bumped transaction
        uint256 txid;
        if (feebumper::CommitTransaction(pwallet, hash, std::move(mtx_out), errors, txid) != feebumper::Result::OK) {
            throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
        }
        result.pushKV("txid", txid.GetHex());
    } else {
        // Add PSBT to result so the user can pass it on
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }

    result.pushKV("fee", ValueFromAmount(new_fee));
    result.pushKV("origfee", ValueFromAmount(old_fee));
    result.pushKV("complete", complete);

    UniValue result_errors(UniValue::VARR);
    for (const std::string& error : errors) {
        result_errors.push_back(error);
    }
    result.pushKV("errors", result_errors);

    return result;
}


UniValue signerdissociate(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1) {
        throw std::runtime_error(
            RPCHelpMan{"signerdissociate",
                "Disossociates external signer from the wallet.\n",
                {
                    {"fingerprint", RPCArg::Type::STR, /* default_val */ "", "Master key fingerprint of signer"},
                },
                RPCResult{"null"},
                RPCExamples{""}
            }.ToString()
        );
    }

    ExternalSigner *signer = GetSignerForJSONRPCRequest(request, 0, pwallet);

    assert(signer != nullptr);
    std::vector<ExternalSigner>::iterator position = std::find(pwallet->m_external_signers.begin(), pwallet->m_external_signers.end(), *signer);
    if (position != pwallet->m_external_signers.end()) pwallet->m_external_signers.erase(position);

    return NullUniValue;
}

static UniValue signerdisplayaddress(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.empty() || request.params.size() > 2) {
        throw std::runtime_error(
            RPCHelpMan{"signerdisplayaddress",
            "Display address on an external signer for verification.\n",
                {
                    {"address",     RPCArg::Type::STR, RPCArg::Optional::NO, /* default_val */ "", "bitcoin address to display"},
                    {"fingerprint", RPCArg::Type::STR, /* default_val */ "", "master key fingerprint of signer"},
                },
                RPCResult{"null"},
                RPCExamples{""}
            }.ToString()
        );
    }

    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    ExternalSigner *signer = GetSignerForJSONRPCRequest(request, 1, pwallet);

    LOCK(pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());

    // Make sure the destination is valid
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    CScript scriptPubKey = GetScriptForDestination(dest);
    auto descriptor = InferDescriptor(scriptPubKey, *pwallet);

    if (!descriptor->IsSolvable()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Key is not solvable");
    }

    // TODO: check that fingerprint and BIP32 path is present (new Descriptor method?)
    // TODO: check that fingerprint matches signer

    signer->displayAddress(descriptor->ToString());

    return UniValue(UniValue::VNULL);
}

std::unique_ptr<Descriptor> ParseDescriptor(const UniValue &descriptor_val, bool must_be_solveable = true, bool must_be_ranged = false) {
    if (!descriptor_val.isStr()) JSONRPCError(RPC_WALLET_ERROR, "Unexpect result");
    FlatSigningProvider provider;
    const std::string desc_str = descriptor_val.getValStr();
    std::unique_ptr<Descriptor> desc = Parse(desc_str, provider, true);
    if (!desc) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Invalid descriptor: %s", desc_str));
    }
    if (!desc->IsRange()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Descriptor must be ranged");
    }
    if (!desc->IsSolvable()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Descriptor must be solvable");
    }
    return desc;
}

UniValue signerfetchkeys(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 3) {
        throw std::runtime_error(
            RPCHelpMan{"signerfetchkeys",
                "Obtains keys from external signer and imports them into the wallet.\n"
                "For interoperability reasons (BIP 44, 49 and 84), it is recommended that you\n"
                "check -addresstype and -changetype settings before calling this.\n"
                "It is also recommended that you continue to use the same address type with this\n"
                "wallet. Call enumeratesigners first.\n",
                {
                    {"account",     RPCArg::Type::NUM, /* default_val */ "0", "BIP32 account to use"},
                    {"fingerprint", RPCArg::Type::STR, /* default_val */ "", "Master key fingerprint of signer"},
                    {"range", RPCArg::Type::RANGE, /* default */ "set by -keypool", "The range of HD chain indexes to import (either end or [begin,end])"},
                },
                RPCResult{
                    "[{ \"success\": true }"
                },
                RPCExamples{""}
            }.ToString()
        );
    }

    ExternalSigner *signer = GetSignerForJSONRPCRequest(request, 1, pwallet);

    int account = 0;
    if (!request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        account = request.params[0].get_int();
    }

    UniValue signer_res = signer->getDescriptors(account);
    if (!signer_res.isObject()) throw JSONRPCError(RPC_WALLET_ERROR, "Unexpect result");
    const UniValue& receive_descriptor_vals = find_value(signer_res, "receive");
    const UniValue& change_descriptor_vals = find_value(signer_res, "internal");
    if (!receive_descriptor_vals.isArray()) throw JSONRPCError(RPC_WALLET_ERROR, "Unexpect result");
    if (!change_descriptor_vals.isArray()) throw JSONRPCError(RPC_WALLET_ERROR, "Unexpect result");

    // Parse and check descriptors
    std::vector<std::unique_ptr<Descriptor>> receive_descriptors;
    std::vector<std::unique_ptr<Descriptor>> change_descriptors;

    for (const UniValue& desc : receive_descriptor_vals.get_array().getValues()) {
        receive_descriptors.push_back(ParseDescriptor(desc, true, true));
    }

    for (const UniValue& desc : change_descriptor_vals.get_array().getValues()) {
        change_descriptors.push_back(ParseDescriptor(desc, true, true));
    }

    uint64_t keypool_target_size = 0;
    keypool_target_size = gArgs.GetArg("-keypool", DEFAULT_KEYPOOL_SIZE);
    int64_t range_begin = 0;
    int64_t range_end = 0;
    if (request.params.size() >= 3 && !request.params[2].isNull()) {
        std::tie(range_begin, range_end) = ParseDescriptorRange(request.params[2]);
    } else {
        range_end = keypool_target_size - 1;
    }
    UniValue range(UniValue::VARR);
    range.push_back(range_begin);
    range.push_back(range_end);

    // Use importmulti to process the descriptors:
    // TODO: extract reusable non-RPC code from importmulti
    UniValue importdata(UniValue::VARR);

    if (keypool_target_size == 0) throw JSONRPCError(RPC_WALLET_ERROR, "-keypool must be > 0");

    UniValue receive_key_data(UniValue::VOBJ);

    // Pick receive descriptor based on -addresstype
    AddressType address_type;
    bool receive_segwit = false;
    switch (pwallet->m_default_address_type) {
        case OutputType::LEGACY: {
        address_type = AddressType::BASE58;
        break;
    }
    case OutputType::P2SH_SEGWIT: {
        address_type = AddressType::BASE58;
        receive_segwit = true;
        break;
    }
    case OutputType::BECH32: {
        address_type = AddressType::BECH32;
        receive_segwit = true;
        break;
    }
    default:
        assert(false);
    }

    std::unique_ptr<Descriptor> match_desc;
    for (auto&& desc : receive_descriptors) {
        if (desc->GetAddressType() == address_type && desc->IsSegWit() == receive_segwit) {
            match_desc = std::move(desc);
            break;
        }
    }

    if (!match_desc) throw JSONRPCError(RPC_WALLET_ERROR, "No descriptor found for wallet address type");
    receive_key_data.pushKV("desc", match_desc->ToString());

    receive_key_data.pushKV("range", range);
    receive_key_data.pushKV("internal", false);
    receive_key_data.pushKV("keypool", true);
    receive_key_data.pushKV("watchonly", true);
    importdata.push_back(receive_key_data);

    UniValue change_key_data(UniValue::VOBJ);

    // Pick receive descriptor based on -changetype
    const OutputType default_change_type = pwallet->m_default_change_type == OutputType::CHANGE_AUTO ? pwallet->m_default_address_type : pwallet->m_default_change_type;
    AddressType change_type;
    bool change_segwit = false;
    switch (default_change_type) {
        case OutputType::LEGACY: {
        change_type = AddressType::BASE58;
        break;
    }
    case OutputType::P2SH_SEGWIT: {
        change_type = AddressType::BASE58;
        change_segwit = true;
        break;
    }
    case OutputType::BECH32: {
        change_type = AddressType::BECH32;
        change_segwit = true;
        break;
    }
    default:
        assert(false);
    }

    match_desc.reset(nullptr);
    for (auto&& desc : change_descriptors) {
        if (desc->GetAddressType() == change_type && desc->IsSegWit() == change_segwit) {
            match_desc = std::move(desc);
            break;
        }
    }

    if (!match_desc) throw JSONRPCError(RPC_WALLET_ERROR, "No descriptor found for wallet change address type");
    change_key_data.pushKV("desc", match_desc->ToString());

    change_key_data.pushKV("range", range);
    change_key_data.pushKV("internal", true);
    change_key_data.pushKV("keypool", true);
    change_key_data.pushKV("watchonly", true);
    importdata.push_back(change_key_data);

    UniValue result(UniValue::VARR);
    {
        auto locked_chain = pwallet->chain().lock();
        const Optional<int> tip_height = locked_chain->getHeight();
        int64_t now = tip_height ? locked_chain->getBlockMedianTimePast(*tip_height) : 0;
        LOCK(pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);
        for (const UniValue& data : importdata.getValues()) {
            // TODO: prevent inserting the same key twice
            result.push_back(ProcessImport(pwallet, data, now));
        }
    }

    // TODO: after the import, fetch a random key from the wallet (part of the import)
    // and ask the signer to sign a message (may require user approval on device).
    // Check the returned signature.
    // This ensures that the device can actually sign with this key and no data
    // corruption occured en route.
    // Note that this doesn't guarantee the device can sign for any script involving this key.

    return result;
}

UniValue signerprocesspsbt(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"signerprocesspsbt",
                "\nSign PSBT inputs using external signer\n"
                "that we can sign for." +
                    HelpRequiringPassphrase(pwallet) + "\n",
                {
                    {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, /* default_val */ "", "The transaction base64 string"},
                    {"fingerprint", RPCArg::Type::STR, /* default_val */ "", "master key fingerprint of signer"},
                },
                RPCResult{
                    "{\n"
                    "  \"hex\" : \"value\",           (string) The hex-encoded network transaction, if complete\n"
                    "  \"psbt\" : \"value\",          (string) The base64-encoded partially signed transaction\n"
                    "  \"complete\" : true|false,     (boolean) If the transaction has a complete set of signatures\n"
                    "  ]\n"
                    "}\n"
                },
                RPCExamples{
                    HelpExampleCli("signerprocesspsbt", "\"psbt\"")
                }
            }.ToString()
        );

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VSTR});

    ExternalSigner *signer = GetSignerForJSONRPCRequest(request, 1, pwallet);

    // Unserialize the transaction
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodeBase64PSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("PSBT decode failed %s", error));
    }

    if( !signer->signTransaction(psbtx, error)) throw JSONRPCError(RPC_WALLET_ERROR, error);

    CMutableTransaction mtx;
    bool complete = FinalizeAndExtractPSBT(psbtx, mtx);
    CDataStream ssPsbtx(SER_NETWORK, PROTOCOL_VERSION);
    ssPsbtx << psbtx;

    UniValue result(UniValue::VOBJ);
    if (complete) {
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        std::string tx_hex;
        ssTx << mtx;
        tx_hex = HexStr(ssTx.str());
        result.pushKV("hex", tx_hex);
    }
    result.pushKV("psbt", EncodeBase64(ssPsbtx.str()));
    result.pushKV("complete", complete);
    return result;
}

UniValue signersend(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
            RPCHelpMan{"signersend",
                "Creates, funds and broadcasts a transaction.\n",
                {
                    {"inputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "A json array of json objects",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                                },
                            },
                        },
                        },
                    {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "a json array with outputs (key-value pairs), where none of the keys are duplicated.\n"
                            "That is, each address can only appear once and there can only be one 'data' object.\n"
                            "For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
                            "                             accepted as second parameter.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the bitcoin address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                                },
                                },
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                                },
                            },
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, /* default */ "0", "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED_NAMED_ARG, "",
                        {
                            {"add_inputs", RPCArg::Type::BOOL, /* default */ "false", "If inputs are specified, automatically include more if they are not enough."},
                            {"changeAddress", RPCArg::Type::STR_HEX, /* default */ "pool address", "The bitcoin address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, /* default */ "random", "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, /* default */ "set by -changetype", "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\"."},
                            {"lockUnspents", RPCArg::Type::BOOL, /* default */ "false", "Lock selected unspent outputs"},
                            {"feeRate", RPCArg::Type::AMOUNT, /* default */ "not set: makes wallet determine the fee", "Set a specific fee rate in " + CURRENCY_UNIT + "/kB"},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, /* default */ "empty array", "A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              Those recipients will receive less bitcoins than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"replaceable", RPCArg::Type::BOOL, /* default */ "fallback to wallet's default", "Marks this transaction as BIP125 replaceable.\n"
                            "                              Allows this transaction to be replaced by a transaction with higher fees"},
                            {"conf_target", RPCArg::Type::NUM, /* default */ "Fallback to wallet's confirmation target", "Confirmation target (in blocks)"},
                            {"estimate_mode", RPCArg::Type::STR, /* default */ "UNSET", "The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\""},
                        },
                        "options"},
                    {"fingerprint", RPCArg::Type::STR, /* default_val */ "", "master key fingerprint of signer"}
                },
                RPCResult{
                    "{\n"
                    "  \"psbt\": \"value\",        (string)  The resulting raw transaction (base64-encoded string)\n"
                    "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                    "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                    "}\n"
                },
                RPCExamples{
                    "\nSend 0.1 BTC\n"
                    + HelpExampleCli("signersend", "\"[]\" \"[{\\\"bc1qkallence7tjawwvy0dwt4twc62qjgaw8f4vlhyd006d99f09\\\": 0.1}]\"")
                }
            }.ToString()
        );

    RPCTypeCheck(request.params, {
        UniValue::VARR,
        UniValueType(), // ARR or OBJ, checked later
        UniValue::VNUM,
        UniValue::VOBJ
        }, true
    );

    // No need to call enumerate first:
    if (pwallet->m_external_signers.empty()) {
        getsigners(pwallet);
    }

    CAmount fee;
    int change_position;
    bool rbf = pwallet->m_signal_rbf;
    if (!request.params[3]["replaceable"].isNull()) {
        rbf = request.params[3]["replaceable"].isTrue();
    }
    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2], rbf);

    CCoinControl coin_control;
    coin_control.fAllowWatchOnly = true;
    // Automatically select coins, unless at least one is manually selected. Can
    // be overriden by options.add_inputs.
    coin_control.m_add_inputs = rawTx.vin.size() == 0;
    FundTransaction(pwallet, rawTx, fee, change_position, request.params[3], coin_control);

    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx);

    // Fill transaction with out data but don't sign
    bool complete_dummy;

    TransactionError fill_psbt_error = FillPSBT(pwallet, psbtx, complete_dummy, 1, false, true);
    if (fill_psbt_error != TransactionError::OK) {
        throw JSONRPCTransactionError(fill_psbt_error);
    }

    // TODO: if more than one signer is known and no fingerprint argument is present,
    //       loop through inputs to find a matching fingerprint.
    ExternalSigner *signer = GetSignerForJSONRPCRequest(request, 4, pwallet);

    // Send to signer and process result
    std::string error;
    if( !signer->signTransaction(psbtx, error)) throw JSONRPCError(RPC_WALLET_ERROR, error);

    CMutableTransaction mtx;
    bool complete = FinalizeAndExtractPSBT(psbtx, mtx);

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);

    UniValue result(UniValue::VOBJ);

    if (complete) {
        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
        std::string err_string;
        UniValue err;
        bool success = pwallet->chain().broadcastTransaction(tx, err_string, DEFAULT_MAX_RAW_TX_FEE, /*relay*/ true);
        if (!success)throw err_string;
        result.pushKV("txid", tx->GetHash().GetHex());
    } else {
        // Add PSBT to result so the user can pass it on
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }

    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);
    result.pushKV("complete", complete);

    return result;
}

// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                                actor (function)                argNames
    //  --------------------- ------------------------          -----------------------         ----------
    { "signer",             "enumeratesigners",                 &enumeratesigners,              {} },
    { "signer",             "signerbumpfee",                    &signerbumpfee,                 {"txid", "options", "fingerprint"} },
    { "signer",             "signerdissociate",                 &signerdissociate,              {"fingerprint"} },
    { "signer",             "signerdisplayaddress",             &signerdisplayaddress,          {"address", "fingerprint"} },
    { "signer",             "signerfetchkeys",                  &signerfetchkeys,               {"account", "fingerprint"} },
    { "signer",             "signerprocesspsbt",                &signerprocesspsbt,             {"psbt", "fingerprint"} },
    { "signer",             "signersend",                       &signersend,                    {"inputs","outputs","locktime","options"} },
};
// clang-format on

void RegisterSignerRPCCommands(interfaces::Chain& chain, std::vector<std::unique_ptr<interfaces::Handler>>& handlers)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        handlers.emplace_back(chain.handleRpc(commands[vcidx]));
}
#endif // HAVE_BOOST_PROCESS
