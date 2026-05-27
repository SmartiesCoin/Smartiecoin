// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <chainparams.h>
#include <deploymentstatus.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <rpc/util.h>
#include <sapling/key_io_sapling.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/standard.h>
#include <serialize.h>
#include <streams.h>
#include <univalue.h>
#include <util/moneystr.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/rpc/util.h>
#include <wallet/sapling_wallet.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <boost/variant/get.hpp>
#include <librustzcash.h>

#include <algorithm>
#include <cstddef>
#include <memory>

namespace wallet {
namespace {

struct SaplingOutputInfo {
    uint256 ovk;
    libzcash::SaplingPaymentAddress address;
    CAmount amount;
    std::array<unsigned char, ZC_MEMO_SIZE> memo;
};

struct SaplingSpendInfo {
    libzcash::SaplingExpandedSpendingKey expsk;
    libzcash::SaplingNote note;
    uint256 alpha;
    uint256 anchor;
    SaplingWitness witness;
};

std::vector<unsigned char> DataStreamToBytes(const CDataStream& stream)
{
    std::vector<unsigned char> bytes;
    bytes.reserve(stream.size());
    std::transform(stream.begin(), stream.end(), std::back_inserter(bytes), [](std::byte b) {
        return std::to_integer<unsigned char>(b);
    });
    return bytes;
}

bool ShieldActiveForNextBlock(const CWallet& wallet)
{
    const auto tip_height = wallet.chain().getHeight();
    if (!tip_height) return false;
    return (*tip_height + 1) >= Params().GetConsensus().nSMTShieldHeight;
}

void EnsureShieldActiveForNextBlock(const CWallet& wallet)
{
    if (!ShieldActiveForNextBlock(wallet)) {
        throw JSONRPCError(RPC_WALLET_ERROR,
            strprintf("Shielded transactions are not active yet. Activation height is %d.",
                      Params().GetConsensus().nSMTShieldHeight));
    }
}

std::array<unsigned char, ZC_MEMO_SIZE> EmptySaplingMemo()
{
    std::array<unsigned char, ZC_MEMO_SIZE> memo = {{0}};
    memo[0] = 0xF6;
    return memo;
}

uint256 Uint256Max()
{
    uint256 value;
    std::fill(value.begin(), value.end(), 0xff);
    return value;
}

OutputDescription DummyShieldedOutput()
{
    OutputDescription output;
    output.cv = Uint256Max();
    output.cmu = Uint256Max();
    output.ephemeralKey = Uint256Max();
    output.encCiphertext = {{0xff}};
    output.outCiphertext = {{0xff}};
    output.zkproof = {{0xff}};
    return output;
}

SpendDescription DummyShieldedSpend()
{
    SpendDescription spend;
    spend.cv = Uint256Max();
    spend.anchor = Uint256Max();
    spend.nullifier = Uint256Max();
    spend.rk = Uint256Max();
    spend.zkproof = {{0xff}};
    spend.spendAuthSig = {{0xff}};
    return spend;
}

bool BuildSaplingOutputDescription(void* ctx, const SaplingOutputInfo& output_info, OutputDescription& output, std::string& error)
{
    libzcash::SaplingNote note(output_info.address, static_cast<uint64_t>(output_info.amount));
    auto cmu = note.cmu();
    if (!cmu) {
        error = "Sapling output commitment failed";
        return false;
    }

    libzcash::SaplingNotePlaintext plaintext(note, output_info.memo);
    auto encryption = plaintext.encrypt(note.pk_d);
    if (!encryption) {
        error = "Sapling output encryption failed";
        return false;
    }

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << output_info.address;
    std::vector<unsigned char> address_bytes = DataStreamToBytes(ss);

    auto encryptor = encryption->second;
    if (!librustzcash_sapling_output_proof(
            ctx,
            encryptor.get_esk().begin(),
            address_bytes.data(),
            note.r.begin(),
            note.value(),
            output.cv.begin(),
            output.zkproof.begin())) {
        error = "Sapling output proof failed";
        return false;
    }

    output.cmu = *cmu;
    output.ephemeralKey = encryptor.get_epk();
    output.encCiphertext = encryption->first;

    libzcash::SaplingOutgoingPlaintext outgoing_plaintext(note.pk_d, encryptor.get_esk());
    output.outCiphertext = outgoing_plaintext.encrypt(output_info.ovk, output.cv, output.cmu, encryptor);
    return true;
}

bool ProveAndSignSaplingData(CMutableTransaction& tx,
                             const std::vector<SaplingSpendInfo>& spend_infos,
                             const std::vector<SaplingOutputInfo>& output_infos,
                             std::string& error)
{
    tx.sapData.vShieldedSpend.clear();
    tx.sapData.vShieldedOutput.clear();
    tx.sapData.bindingSig = {{0}};

    if (spend_infos.empty() && output_infos.empty()) return true;

    void* ctx = librustzcash_sapling_proving_ctx_init();
    if (ctx == nullptr) {
        error = "Failed to initialize Sapling proving context";
        return false;
    }

    for (const SaplingOutputInfo& output_info : output_infos) {
        OutputDescription output;
        if (!BuildSaplingOutputDescription(ctx, output_info, output, error)) {
            librustzcash_sapling_proving_ctx_free(ctx);
            return false;
        }
        tx.sapData.vShieldedOutput.push_back(output);
    }

    for (const SaplingSpendInfo& spend_info : spend_infos) {
        auto cmu = spend_info.note.cmu();
        auto nullifier = spend_info.note.nullifier(spend_info.expsk.full_viewing_key(), spend_info.witness.position());
        if (!cmu || !nullifier) {
            librustzcash_sapling_proving_ctx_free(ctx);
            error = "Sapling spend note is invalid";
            return false;
        }

        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << spend_info.witness.path();
        std::vector<unsigned char> witness = DataStreamToBytes(ss);

        SpendDescription spend;
        if (!librustzcash_sapling_spend_proof(
                ctx,
                spend_info.expsk.full_viewing_key().ak.begin(),
                spend_info.expsk.nsk.begin(),
                spend_info.note.d.data(),
                spend_info.note.r.begin(),
                spend_info.alpha.begin(),
                spend_info.note.value(),
                spend_info.anchor.begin(),
                witness.data(),
                spend.cv.begin(),
                spend.rk.begin(),
                spend.zkproof.begin())) {
            librustzcash_sapling_proving_ctx_free(ctx);
            error = "Sapling spend proof failed";
            return false;
        }

        spend.anchor = spend_info.anchor;
        spend.nullifier = *nullifier;
        tx.sapData.vShieldedSpend.push_back(spend);
    }

    uint256 data_to_be_signed;
    try {
        CScript script_code;
        data_to_be_signed = SignatureHash(script_code, tx, NOT_AN_INPUT, SIGHASH_ALL, 0, SigVersion::SAPLING);
    } catch (const std::exception& e) {
        librustzcash_sapling_proving_ctx_free(ctx);
        error = strprintf("Could not construct Sapling signature hash: %s", e.what());
        return false;
    }

    for (size_t i = 0; i < spend_infos.size(); ++i) {
        if (!librustzcash_sapling_spend_sig(
                spend_infos[i].expsk.ask.begin(),
                spend_infos[i].alpha.begin(),
                data_to_be_signed.begin(),
                tx.sapData.vShieldedSpend[i].spendAuthSig.begin())) {
            librustzcash_sapling_proving_ctx_free(ctx);
            error = "Sapling spend authorization signature failed";
            return false;
        }
    }

    if (!librustzcash_sapling_binding_sig(
            ctx,
            tx.sapData.valueBalance,
            data_to_be_signed.begin(),
            tx.sapData.bindingSig.begin())) {
        librustzcash_sapling_proving_ctx_free(ctx);
        error = "Sapling binding signature failed";
        return false;
    }

    librustzcash_sapling_proving_ctx_free(ctx);
    return true;
}

void AddDummySaplingData(CMutableTransaction& tx, size_t spend_count, size_t output_count)
{
    tx.sapData.vShieldedSpend.assign(spend_count, DummyShieldedSpend());
    tx.sapData.vShieldedOutput.assign(output_count, DummyShieldedOutput());
    if (spend_count != 0 || output_count != 0) {
        tx.sapData.bindingSig = {{0xff}};
    }
}

CAmount EstimateShieldedFee(CWallet& wallet, const CMutableTransaction& tx, const CCoinControl& coin_control, const Optional<CAmount>& explicit_fee)
{
    if (explicit_fee) {
        if (*explicit_fee < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Fee cannot be negative");
        return *explicit_fee;
    }

    FeeCalculation fee_calc;
    const unsigned int tx_size = ::GetSerializeSize(tx, PROTOCOL_VERSION);
    CAmount fee = GetMinimumFee(wallet, tx_size, coin_control, &fee_calc);
    fee = std::max(fee, wallet.chain().relayMinFee().GetFee(tx_size));
    if (fee > wallet.m_default_max_tx_fee) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Calculated fee %s exceeds wallet max tx fee %s",
            FormatMoney(fee), FormatMoney(wallet.m_default_max_tx_fee)));
    }
    return fee;
}

libzcash::SaplingPaymentAddress DecodeShieldAddressOrThrow(const std::string& encoded)
{
    const auto address = KeyIO::DecodeSaplingPaymentAddress(encoded);
    if (!address) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Smartiecoin shielded address");
    }
    return *address;
}

std::string EncodeShieldAddress(const libzcash::SaplingPaymentAddress& address)
{
    return KeyIO::EncodePaymentAddress(libzcash::PaymentAddress(address));
}

UniValue NoteToJSON(const SaplingNoteEntry& note, SaplingWallet& sapling_wallet)
{
    UniValue entry(UniValue::VOBJ);
    entry.pushKV("txid", note.op.hash.GetHex());
    entry.pushKV("output", static_cast<int64_t>(note.op.n));
    entry.pushKV("address", EncodeShieldAddress(note.address));
    entry.pushKV("amount", ValueFromAmount(static_cast<CAmount>(note.note.value())));
    entry.pushKV("confirmations", note.confirmations);
    entry.pushKV("spendable", sapling_wallet.HaveSpendingKeyForPaymentAddress(note.address));
    entry.pushKV("spent", sapling_wallet.IsSaplingSpent(note.op));
    return entry;
}

CTransactionRef CommitShieldedTransaction(CWallet& wallet, CMutableTransaction&& tx, bool verbose)
{
    CTransactionRef tx_ref = MakeTransactionRef(std::move(tx));
    wallet.CommitTransaction(tx_ref, {}, {} /* orderForm */);
    return tx_ref;
}

UniValue ShieldedSendResult(const CTransactionRef& tx, CAmount fee, bool verbose)
{
    if (!verbose) return tx->GetHash().GetHex();

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("hex", EncodeHexTx(*tx));
    return result;
}

CTxDestination DecodeTransparentAddressOrThrow(const std::string& encoded)
{
    std::string error;
    CTxDestination dest = DecodeDestination(encoded, error);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error.empty() ? "Invalid Smartiecoin transparent address" : error);
    }
    return dest;
}

bool TryDecodeShieldAddress(const std::string& encoded, libzcash::SaplingPaymentAddress& address_out)
{
    const auto decoded = KeyIO::DecodeSaplingPaymentAddress(encoded);
    if (!decoded) return false;
    address_out = *decoded;
    return true;
}

void SelectTransparentInputs(CWallet& wallet,
                             const CCoinControl& coin_control,
                             CAmount target,
                             std::vector<COutput>& selected,
                             CAmount& selected_value)
{
    selected.clear();
    selected_value = 0;
    std::vector<COutput> available = AvailableCoins(wallet, &coin_control).all();
    std::sort(available.begin(), available.end(), [](const COutput& a, const COutput& b) {
        if (a.depth != b.depth) return a.depth > b.depth;
        return a.txout.nValue > b.txout.nValue;
    });

    for (const COutput& output : available) {
        if (!output.spendable || !output.safe) continue;
        selected.push_back(output);
        selected_value += output.txout.nValue;
        if (selected_value >= target) return;
    }

    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("Insufficient transparent funds: need %s, available %s",
        FormatMoney(target), FormatMoney(selected_value)));
}

void SelectSaplingNotes(CWallet& wallet,
                        const Optional<libzcash::SaplingPaymentAddress>& from,
                        int min_depth,
                        CAmount target,
                        std::vector<SaplingSpendableNoteEntry>& selected,
                        CAmount& selected_value)
{
    selected.clear();
    selected_value = 0;

    std::vector<SaplingSpendableNoteEntry> available;
    wallet.GetSaplingWallet().GetSpendableNotes(available, from, min_depth);
    std::sort(available.begin(), available.end(), [](const SaplingSpendableNoteEntry& a, const SaplingSpendableNoteEntry& b) {
        if (a.confirmations != b.confirmations) return a.confirmations > b.confirmations;
        return a.note.value() > b.note.value();
    });

    for (const SaplingSpendableNoteEntry& note : available) {
        selected.push_back(note);
        selected_value += static_cast<CAmount>(note.note.value());
        if (selected_value >= target) return;
    }

    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("Insufficient shielded funds: need %s, available %s",
        FormatMoney(target), FormatMoney(selected_value)));
}

CMutableTransaction BuildTransparentToShieldTemplate(const std::vector<COutput>& selected,
                                                     const CScript& change_script,
                                                     CAmount amount,
                                                     CAmount fee,
                                                     CAmount selected_value,
                                                     size_t shield_output_count,
                                                     bool allow_change)
{
    CMutableTransaction tx;
    tx.nVersion = CTransaction::SHIELDED_VERSION;
    tx.nType = TRANSACTION_NORMAL;

    for (const COutput& output : selected) {
        tx.vin.emplace_back(output.outpoint);
    }

    tx.sapData.valueBalance = -amount;
    const CAmount change = selected_value - amount - fee;
    if (change < 0) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Selected inputs do not cover amount plus fee");
    if (allow_change && change > 0) {
        tx.vout.emplace_back(change, change_script);
    }
    AddDummySaplingData(tx, /*spend_count=*/0, shield_output_count);
    return tx;
}

CMutableTransaction BuildShieldFromNotesTemplate(const std::vector<SaplingSpendableNoteEntry>& selected_notes,
                                                 const std::vector<CTxOut>& transparent_outputs,
                                                 CAmount shielded_value_out,
                                                 CAmount fee,
                                                 CAmount selected_value,
                                                 size_t shield_output_count)
{
    CMutableTransaction tx;
    tx.nVersion = CTransaction::SHIELDED_VERSION;
    tx.nType = TRANSACTION_NORMAL;
    tx.vout = transparent_outputs;

    CAmount transparent_value_out = 0;
    for (const CTxOut& out : transparent_outputs) {
        transparent_value_out += out.nValue;
    }

    const CAmount change = selected_value - transparent_value_out - shielded_value_out - fee;
    if (change < 0) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Selected shielded notes do not cover amount plus fee");
    tx.sapData.valueBalance = selected_value - shielded_value_out - change;
    AddDummySaplingData(tx, selected_notes.size(), shield_output_count);
    return tx;
}

} // namespace

RPCHelpMan getnewshieldaddress()
{
    return RPCHelpMan{"getnewshieldaddress",
        "\nReturns a new Smartiecoin shielded Sapling address.\n",
        {
            {"label", RPCArg::Type::STR, RPCArg::Default{""}, "Optional label kept for CLI compatibility; shielded labels are not displayed in the legacy address book yet."},
        },
        RPCResult{RPCResult::Type::STR, "address", "The new shielded address"},
        RPCExamples{HelpExampleCli("getnewshieldaddress", "") + HelpExampleRpc("getnewshieldaddress", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);
            LOCK(pwallet->cs_wallet);

            std::string label;
            if (!request.params[0].isNull()) label = LabelFromValue(request.params[0]);
            return EncodeShieldAddress(pwallet->GetSaplingWallet().GenerateNewAddress(label));
        }};
}

RPCHelpMan listshieldaddresses()
{
    return RPCHelpMan{"listshieldaddresses",
        "\nLists shielded Sapling addresses known by this wallet.\n",
        {},
        RPCResult{RPCResult::Type::ARR, "addresses", "Known shielded addresses", {{RPCResult::Type::STR, "address", "A shielded address"}}},
        RPCExamples{HelpExampleCli("listshieldaddresses", "") + HelpExampleRpc("listshieldaddresses", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            LOCK(pwallet->cs_wallet);
            std::set<libzcash::SaplingPaymentAddress> addresses;
            pwallet->GetSaplingWallet().GetPaymentAddresses(addresses);
            UniValue result(UniValue::VARR);
            for (const auto& address : addresses) {
                result.push_back(EncodeShieldAddress(address));
            }
            return result;
        }};
}

RPCHelpMan getshieldbalance()
{
    return RPCHelpMan{"getshieldbalance",
        "\nReturns the wallet's shielded Sapling balance, optionally filtered by shielded address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"all wallet shielded addresses"}, "Optional shielded address to filter."},
            {"minconf", RPCArg::Type::NUM, RPCArg::Default{1}, "Only include notes confirmed at least this many times."},
            {"include_unspendable", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include notes for which this wallet only has viewing authority."},
        },
        RPCResult{RPCResult::Type::STR_AMOUNT, "amount", "Shielded balance in SMT"},
        RPCExamples{HelpExampleCli("getshieldbalance", "") + HelpExampleRpc("getshieldbalance", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);

            Optional<libzcash::SaplingPaymentAddress> address = nullopt;
            if (!request.params[0].isNull()) address = DecodeShieldAddressOrThrow(request.params[0].get_str());
            const int min_depth = request.params[1].isNull() ? 1 : request.params[1].getInt<int>();
            const bool include_unspendable = !request.params[2].isNull() && request.params[2].get_bool();
            return ValueFromAmount(pwallet->GetSaplingWallet().GetBalance(address, min_depth, /*ignore_unspendable=*/!include_unspendable));
        }};
}

RPCHelpMan listshieldunspent()
{
    return RPCHelpMan{"listshieldunspent",
        "\nReturns unspent shielded Sapling notes in the wallet.\n",
        {
            {"minconf", RPCArg::Type::NUM, RPCArg::Default{1}, "Only include notes confirmed at least this many times."},
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"all wallet shielded addresses"}, "Optional shielded address to filter."},
            {"include_unspendable", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include notes for which this wallet only has viewing authority."},
        },
        RPCResult{RPCResult::Type::ARR, "notes", "Unspent shielded notes", {
            {RPCResult::Type::OBJ, "note", "A shielded note", {
                {RPCResult::Type::STR_HEX, "txid", "Transaction id"},
                {RPCResult::Type::NUM, "output", "Shielded output index"},
                {RPCResult::Type::STR, "address", "Shielded address"},
                {RPCResult::Type::STR_AMOUNT, "amount", "Note amount"},
                {RPCResult::Type::NUM, "confirmations", "Confirmations"},
                {RPCResult::Type::BOOL, "spendable", "Whether the wallet has the spending key"},
                {RPCResult::Type::BOOL, "spent", "Whether this note is spent"},
            }},
        }},
        RPCExamples{HelpExampleCli("listshieldunspent", "") + HelpExampleRpc("listshieldunspent", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);

            const int min_depth = request.params[0].isNull() ? 1 : request.params[0].getInt<int>();
            Optional<libzcash::SaplingPaymentAddress> address = nullopt;
            if (!request.params[1].isNull()) address = DecodeShieldAddressOrThrow(request.params[1].get_str());
            const bool include_unspendable = !request.params[2].isNull() && request.params[2].get_bool();

            std::vector<SaplingNoteEntry> notes;
            pwallet->GetSaplingWallet().GetFilteredNotes(notes, address, min_depth, /*ignore_spent=*/true, /*require_spending_key=*/!include_unspendable);

            UniValue result(UniValue::VARR);
            for (const auto& note : notes) {
                result.push_back(NoteToJSON(note, pwallet->GetSaplingWallet()));
            }
            return result;
        }};
}

RPCHelpMan sendtoshieldaddress()
{
    return RPCHelpMan{"sendtoshieldaddress",
        "\nSends transparent wallet funds to a Smartiecoin shielded Sapling address.\n",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The shielded Sapling recipient address."},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in SMT to shield."},
            {"minconf", RPCArg::Type::NUM, RPCArg::Default{1}, "Only use transparent inputs confirmed at least this many times."},
            {"fee", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"automatic"}, "Optional absolute transaction fee in SMT."},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "Return an object with txid, fee, and hex."},
        },
        RPCResult{RPCResult::Type::ANY, "result", "The transaction id, or an object when verbose=true."},
        RPCExamples{HelpExampleCli("sendtoshieldaddress", "\"smtsapling1...\" 10")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            const auto recipient = DecodeShieldAddressOrThrow(request.params[0].get_str());
            const CAmount amount = AmountFromValue(request.params[1]);
            if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be greater than zero");
            const int min_depth = request.params[2].isNull() ? 1 : request.params[2].getInt<int>();
            Optional<CAmount> explicit_fee = nullopt;
            if (!request.params[3].isNull()) explicit_fee = AmountFromValue(request.params[3]);
            const bool verbose = !request.params[4].isNull() && request.params[4].get_bool();

            pwallet->BlockUntilSyncedToCurrentChain();
            EnsureWalletIsUnlocked(*pwallet);
            EnsureShieldActiveForNextBlock(*pwallet);
            LOCK(pwallet->cs_wallet);
            const CFeeRate dust_relay_fee = pwallet->chain().relayDustFee();

            CCoinControl coin_control;
            coin_control.m_min_depth = min_depth;

            ReserveDestination reservedest(pwallet.get());
            auto op_dest = reservedest.GetReservedDestination(/*internal=*/true);
            if (!op_dest) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Could not reserve a transparent change address");
            }
            const CScript change_script = GetScriptForDestination(*op_dest);

            std::vector<COutput> selected;
            CAmount selected_value = 0;
            CAmount fee = explicit_fee ? *explicit_fee : 0;
            bool use_change = false;

            for (int attempt = 0; attempt < 10; ++attempt) {
                SelectTransparentInputs(*pwallet, coin_control, amount + fee, selected, selected_value);

                CAmount change = selected_value - amount - fee;
                CTxOut change_out(change, change_script);
                use_change = change > 0 && !IsDust(change_out, dust_relay_fee);
                CMutableTransaction tx_template = BuildTransparentToShieldTemplate(
                    selected, change_script, amount, fee, selected_value, /*shield_output_count=*/1, use_change);

                const CAmount next_fee = EstimateShieldedFee(*pwallet, tx_template, coin_control, explicit_fee);
                if (explicit_fee || next_fee == fee) {
                    fee = next_fee;
                    break;
                }
                fee = next_fee;
                if (attempt == 9) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Fee calculation did not converge");
                }
            }

            CAmount change = selected_value - amount - fee;
            if (change < 0) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient transparent funds");
            CTxOut change_out(change, change_script);
            use_change = change > 0 && !IsDust(change_out, dust_relay_fee);
            if (!use_change) fee += change;

            CMutableTransaction tx;
            tx.nVersion = CTransaction::SHIELDED_VERSION;
            tx.nType = TRANSACTION_NORMAL;
            for (const COutput& output : selected) {
                tx.vin.emplace_back(output.outpoint);
            }
            if (use_change) {
                tx.vout.emplace_back(change, change_script);
            }
            tx.sapData.valueBalance = -amount;

            std::vector<SaplingOutputInfo> shield_outputs{{uint256::ZERO, recipient, amount, EmptySaplingMemo()}};
            std::string error;
            if (!ProveAndSignSaplingData(tx, {}, shield_outputs, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            if (!pwallet->SignTransaction(tx)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to sign transparent inputs");
            }

            CTransactionRef tx_ref = CommitShieldedTransaction(*pwallet, std::move(tx), verbose);
            if (use_change) reservedest.KeepDestination();
            return ShieldedSendResult(tx_ref, fee, verbose);
        }};
}

RPCHelpMan sendfromshieldaddress()
{
    return RPCHelpMan{"sendfromshieldaddress",
        "\nSends shielded Sapling funds to either a transparent address or another shielded address.\n",
        {
            {"fromaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The wallet shielded address to spend from, or \"*\" for all wallet shielded notes."},
            {"toaddress", RPCArg::Type::STR, RPCArg::Optional::NO, "A transparent Smartiecoin address or shielded Sapling address."},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in SMT to send."},
            {"minconf", RPCArg::Type::NUM, RPCArg::Default{1}, "Only use shielded notes confirmed at least this many times."},
            {"fee", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"automatic"}, "Optional absolute transaction fee in SMT."},
            {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "Return an object with txid, fee, and hex."},
        },
        RPCResult{RPCResult::Type::ANY, "result", "The transaction id, or an object when verbose=true."},
        RPCExamples{HelpExampleCli("sendfromshieldaddress", "\"*\" \"ST...\" 1")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            Optional<libzcash::SaplingPaymentAddress> from = nullopt;
            if (request.params[0].get_str() != "*") {
                from = DecodeShieldAddressOrThrow(request.params[0].get_str());
            }

            libzcash::SaplingPaymentAddress shield_recipient;
            const bool to_shielded = TryDecodeShieldAddress(request.params[1].get_str(), shield_recipient);
            CTxDestination transparent_recipient;
            if (!to_shielded) {
                transparent_recipient = DecodeTransparentAddressOrThrow(request.params[1].get_str());
            }

            const CAmount amount = AmountFromValue(request.params[2]);
            if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Amount must be greater than zero");
            const int min_depth = request.params[3].isNull() ? 1 : request.params[3].getInt<int>();
            Optional<CAmount> explicit_fee = nullopt;
            if (!request.params[4].isNull()) explicit_fee = AmountFromValue(request.params[4]);
            const bool verbose = !request.params[5].isNull() && request.params[5].get_bool();

            pwallet->BlockUntilSyncedToCurrentChain();
            EnsureWalletIsUnlocked(*pwallet);
            EnsureShieldActiveForNextBlock(*pwallet);
            LOCK(pwallet->cs_wallet);
            const CFeeRate dust_relay_fee = pwallet->chain().relayDustFee();

            CCoinControl coin_control;
            std::vector<CTxOut> transparent_outputs;
            if (!to_shielded) {
                CTxOut txout(amount, GetScriptForDestination(transparent_recipient));
                if (IsDust(txout, dust_relay_fee)) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "Transparent recipient amount is dust");
                }
                transparent_outputs.push_back(txout);
            }

            std::vector<SaplingSpendableNoteEntry> selected_notes;
            CAmount selected_value = 0;
            CAmount fee = explicit_fee ? *explicit_fee : 0;
            size_t shield_output_count = to_shielded ? 1 : 0;

            for (int attempt = 0; attempt < 10; ++attempt) {
                SelectSaplingNotes(*pwallet, from, min_depth, amount + fee, selected_notes, selected_value);
                const CAmount change = selected_value - amount - fee;
                shield_output_count = (to_shielded ? 1 : 0) + (change > 0 ? 1 : 0);
                CMutableTransaction tx_template = BuildShieldFromNotesTemplate(
                    selected_notes,
                    transparent_outputs,
                    to_shielded ? amount : 0,
                    fee,
                    selected_value,
                    shield_output_count);
                const CAmount next_fee = EstimateShieldedFee(*pwallet, tx_template, coin_control, explicit_fee);
                if (explicit_fee || next_fee == fee) {
                    fee = next_fee;
                    break;
                }
                fee = next_fee;
                if (attempt == 9) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Fee calculation did not converge");
                }
            }

            const CAmount change = selected_value - amount - fee;
            if (change < 0) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient shielded funds");

            std::vector<SaplingSpendInfo> shield_spends;
            shield_spends.reserve(selected_notes.size());
            uint256 ovk = uint256::ZERO;
            for (const SaplingSpendableNoteEntry& note : selected_notes) {
                libzcash::SaplingExtendedSpendingKey sk;
                if (!pwallet->GetSaplingWallet().GetSpendingKeyForPaymentAddress(note.address, sk)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Missing spending key for selected shielded note");
                }
                SaplingSpendInfo spend{sk.expsk, note.note, uint256::ZERO, note.anchor, note.witness};
                librustzcash_sapling_generate_r(spend.alpha.begin());
                if (ovk.IsNull()) ovk = sk.expsk.full_viewing_key().ovk;
                shield_spends.push_back(spend);
            }

            std::vector<SaplingOutputInfo> shield_outputs;
            CAmount shielded_value_out = 0;
            if (to_shielded) {
                shield_outputs.push_back({ovk, shield_recipient, amount, EmptySaplingMemo()});
                shielded_value_out += amount;
            }
            if (change > 0) {
                const libzcash::SaplingPaymentAddress change_address = from ? *from : selected_notes.front().address;
                shield_outputs.push_back({ovk, change_address, change, EmptySaplingMemo()});
                shielded_value_out += change;
            }

            CMutableTransaction tx;
            tx.nVersion = CTransaction::SHIELDED_VERSION;
            tx.nType = TRANSACTION_NORMAL;
            tx.vout = transparent_outputs;
            tx.sapData.valueBalance = selected_value - shielded_value_out;

            std::string error;
            if (!ProveAndSignSaplingData(tx, shield_spends, shield_outputs, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }

            CTransactionRef tx_ref = CommitShieldedTransaction(*pwallet, std::move(tx), verbose);
            return ShieldedSendResult(tx_ref, fee, verbose);
        }};
}

RPCHelpMan exportsaplingkey()
{
    return RPCHelpMan{"exportsaplingkey",
        "\nExports the Sapling spending key for a shielded address. Keep it private.\n",
        {{"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The shielded address."}},
        RPCResult{RPCResult::Type::STR, "key", "The encoded Sapling spending key"},
        RPCExamples{HelpExampleCli("exportsaplingkey", "\"shieldaddress\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);
            LOCK(pwallet->cs_wallet);

            const auto address = DecodeShieldAddressOrThrow(request.params[0].get_str());
            libzcash::SaplingExtendedSpendingKey sk;
            if (!pwallet->GetSaplingWallet().GetSpendingKeyForPaymentAddress(address, sk)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Spending key for this shielded address is not known");
            }
            return KeyIO::EncodeSpendingKey(libzcash::SpendingKey(sk));
        }};
}

RPCHelpMan exportsaplingviewingkey()
{
    return RPCHelpMan{"exportsaplingviewingkey",
        "\nExports the Sapling full viewing key for a shielded address.\n",
        {{"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The shielded address."}},
        RPCResult{RPCResult::Type::STR, "key", "The encoded Sapling viewing key"},
        RPCExamples{HelpExampleCli("exportsaplingviewingkey", "\"shieldaddress\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            LOCK(pwallet->cs_wallet);
            const auto address = DecodeShieldAddressOrThrow(request.params[0].get_str());
            libzcash::SaplingIncomingViewingKey ivk;
            libzcash::SaplingExtendedFullViewingKey extfvk;
            if (!pwallet->GetSaplingWallet().GetIncomingViewingKey(address, ivk) ||
                !pwallet->GetSaplingWallet().GetFullViewingKey(ivk, extfvk)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Viewing key for this shielded address is not known");
            }
            return KeyIO::EncodeViewingKey(libzcash::ViewingKey(extfvk));
        }};
}

RPCHelpMan importsaplingkey()
{
    return RPCHelpMan{"importsaplingkey",
        "\nImports a Sapling spending key.\n",
        {
            {"key", RPCArg::Type::STR, RPCArg::Optional::NO, "The Sapling spending key from exportsaplingkey."},
            {"rescan", RPCArg::Type::BOOL, RPCArg::Default{true}, "Rescan wallet transactions after import."},
        },
        RPCResult{RPCResult::Type::OBJ, "result", "Import result", {
            {RPCResult::Type::STR, "address", "Default shielded address for the imported key"},
            {RPCResult::Type::BOOL, "rescan", "Whether wallet transactions were rescanned"},
        }},
        RPCExamples{HelpExampleCli("importsaplingkey", "\"saplingsecret...\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            EnsureWalletIsUnlocked(*pwallet);
            LOCK(pwallet->cs_wallet);

            const auto key = KeyIO::DecodeSpendingKey(request.params[0].get_str());
            const auto* sk = boost::get<libzcash::SaplingExtendedSpendingKey>(&key);
            if (!sk) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Sapling spending key");

            if (!pwallet->GetSaplingWallet().AddSpendingKey(*sk, /*create_time=*/1)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Failed to import Sapling spending key");
            }
            const bool rescan = request.params[1].isNull() ? true : request.params[1].get_bool();
            if (rescan) pwallet->GetSaplingWallet().RescanWalletTransactions();

            UniValue result(UniValue::VOBJ);
            result.pushKV("address", EncodeShieldAddress(sk->DefaultAddress()));
            result.pushKV("rescan", rescan);
            return result;
        }};
}

} // namespace wallet
