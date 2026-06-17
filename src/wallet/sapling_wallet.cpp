// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/sapling_wallet.h>

#include <chainparams.h>
#include <interfaces/chain.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <sapling/key_io_sapling.h>
#include <serialize.h>
#include <streams.h>
#include <support/cleanse.h>
#include <util/check.h>
#include <util/time.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <cstddef>
#include <limits>

using interfaces::FoundBlock;

namespace wallet {
namespace {

CKeyingMaterial SerializeSaplingSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << sk;

    CKeyingMaterial secret;
    secret.resize(ss.size());
    std::transform(ss.begin(), ss.end(), secret.begin(), [](std::byte b) {
        return std::to_integer<unsigned char>(b);
    });
    return secret;
}

bool DeserializeSaplingSpendingKey(const CKeyingMaterial& secret, libzcash::SaplingExtendedSpendingKey& sk)
{
    try {
        CDataStream ss(Span<const uint8_t>(secret.data(), secret.size()), SER_NETWORK, PROTOCOL_VERSION);
        ss >> sk;
        return ss.empty();
    } catch (const std::exception&) {
        return false;
    }
}

void AppendCommitmentToWalletWitnesses(CWallet& wallet, const uint256& cmu)
{
    AssertLockHeld(wallet.cs_wallet);
    for (auto& [txid, wtx] : wallet.mapWallet) {
        for (auto& [op, nd] : wtx.mapSaplingNoteData) {
            if (!nd.IsMyNote() || nd.witnesses.empty()) continue;
            nd.witnesses.front().append(cmu);
        }
    }
}

} // namespace

libzcash::SaplingPaymentAddress SaplingWallet::GenerateNewAddress(const std::string& label)
{
    (void)label;
    AssertLockHeld(m_wallet.cs_wallet);

    for (int tries = 0; tries < 100; ++tries) {
        HDSeed seed = HDSeed::Random(32);
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(seed);
        auto addr = sk.DefaultAddress();
        if (m_incoming_viewing_keys.count(addr) != 0) continue;
        if (!AddSpendingKey(sk, GetTime())) {
            throw std::runtime_error("failed to save Sapling spending key");
        }
        return addr;
    }
    throw std::runtime_error("failed to generate a unique Sapling address");
}

bool SaplingWallet::AddSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk, int64_t create_time, WalletBatch* batch)
{
    AssertLockHeld(m_wallet.cs_wallet);

    const auto extfvk = sk.ToXFVK();
    const auto ivk = extfvk.fvk.in_viewing_key();
    const auto address = extfvk.DefaultAddress();
    CKeyMetadata metadata(create_time);

    WalletBatch local_batch(m_wallet.GetDatabase());
    WalletBatch& db = batch ? *batch : local_batch;

    m_full_viewing_keys[ivk] = extfvk;
    m_incoming_viewing_keys[address] = ivk;
    m_key_metadata[ivk] = metadata;

    if (m_wallet.IsCrypted()) {
        if (m_wallet.IsLocked()) return false;
        CKeyingMaterial secret = SerializeSaplingSpendingKey(sk);
        std::vector<unsigned char> crypted_secret;
        const bool encrypted = EncryptSecret(m_wallet.vMasterKey, secret, extfvk.fvk.GetFingerprint(), crypted_secret);
        memory_cleanse(secret.data(), secret.size());
        if (!encrypted) return false;
        m_crypted_spending_keys[extfvk] = crypted_secret;
        if (!db.WriteCryptedSaplingZKey(extfvk, crypted_secret, metadata)) return false;
    } else {
        m_spending_keys[extfvk] = sk;
        if (!db.WriteSaplingZKey(ivk, sk, metadata)) return false;
    }

    return db.WriteSaplingPaymentAddress(address, ivk);
}

bool SaplingWallet::LoadSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk)
{
    AssertLockHeld(m_wallet.cs_wallet);
    const auto extfvk = sk.ToXFVK();
    const auto ivk = extfvk.fvk.in_viewing_key();
    m_full_viewing_keys[ivk] = extfvk;
    m_incoming_viewing_keys[extfvk.DefaultAddress()] = ivk;
    m_spending_keys[extfvk] = sk;
    return true;
}

bool SaplingWallet::LoadCryptedSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk, const std::vector<unsigned char>& crypted_secret)
{
    AssertLockHeld(m_wallet.cs_wallet);
    const auto ivk = extfvk.fvk.in_viewing_key();
    m_full_viewing_keys[ivk] = extfvk;
    m_incoming_viewing_keys[extfvk.DefaultAddress()] = ivk;
    m_crypted_spending_keys[extfvk] = crypted_secret;
    return true;
}

bool SaplingWallet::LoadKeyMetadata(const libzcash::SaplingIncomingViewingKey& ivk, const CKeyMetadata& meta)
{
    AssertLockHeld(m_wallet.cs_wallet);
    m_key_metadata[ivk] = meta;
    return true;
}

bool SaplingWallet::LoadPaymentAddress(const libzcash::SaplingPaymentAddress& address, const libzcash::SaplingIncomingViewingKey& ivk)
{
    AssertLockHeld(m_wallet.cs_wallet);
    m_incoming_viewing_keys[address] = ivk;
    return true;
}

bool SaplingWallet::EncryptKeys(const CKeyingMaterial& master_key, WalletBatch& batch)
{
    AssertLockHeld(m_wallet.cs_wallet);

    for (const auto& [extfvk, sk] : m_spending_keys) {
        CKeyingMaterial secret = SerializeSaplingSpendingKey(sk);
        std::vector<unsigned char> crypted_secret;
        const bool encrypted = EncryptSecret(master_key, secret, extfvk.fvk.GetFingerprint(), crypted_secret);
        memory_cleanse(secret.data(), secret.size());
        if (!encrypted) return false;

        const auto ivk = extfvk.fvk.in_viewing_key();
        auto meta_it = m_key_metadata.find(ivk);
        CKeyMetadata metadata = meta_it != m_key_metadata.end() ? meta_it->second : CKeyMetadata(GetTime());
        if (!batch.WriteCryptedSaplingZKey(extfvk, crypted_secret, metadata)) return false;
        m_crypted_spending_keys[extfvk] = crypted_secret;
    }

    m_spending_keys.clear();
    return true;
}

bool SaplingWallet::CheckDecryptionKey(const CKeyingMaterial& master_key) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    for (const auto& [extfvk, crypted_secret] : m_crypted_spending_keys) {
        CKeyingMaterial secret;
        if (!DecryptSecret(master_key, crypted_secret, extfvk.fvk.GetFingerprint(), secret)) return false;
        libzcash::SaplingExtendedSpendingKey sk;
        const bool decoded = DeserializeSaplingSpendingKey(secret, sk);
        memory_cleanse(secret.data(), secret.size());
        if (!decoded || !(sk.ToXFVK() == extfvk)) return false;
    }
    return true;
}

bool SaplingWallet::HaveSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    return m_spending_keys.count(extfvk) != 0 || m_crypted_spending_keys.count(extfvk) != 0;
}

bool SaplingWallet::HaveSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    libzcash::SaplingIncomingViewingKey ivk;
    if (!GetIncomingViewingKey(address, ivk)) return false;
    libzcash::SaplingExtendedFullViewingKey extfvk;
    return GetFullViewingKey(ivk, extfvk) && HaveSpendingKey(extfvk);
}

Optional<libzcash::SaplingExtendedSpendingKey> SaplingWallet::GetSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk) const
{
    AssertLockHeld(m_wallet.cs_wallet);

    auto plain_it = m_spending_keys.find(extfvk);
    if (plain_it != m_spending_keys.end()) return plain_it->second;

    auto crypted_it = m_crypted_spending_keys.find(extfvk);
    if (crypted_it == m_crypted_spending_keys.end() || m_wallet.IsLocked()) return nullopt;

    CKeyingMaterial secret;
    if (!DecryptSecret(m_wallet.vMasterKey, crypted_it->second, extfvk.fvk.GetFingerprint(), secret)) return nullopt;
    libzcash::SaplingExtendedSpendingKey sk;
    const bool decoded = DeserializeSaplingSpendingKey(secret, sk);
    memory_cleanse(secret.data(), secret.size());
    if (!decoded || !(sk.ToXFVK() == extfvk)) return nullopt;
    return sk;
}

bool SaplingWallet::GetSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address, libzcash::SaplingExtendedSpendingKey& sk_out) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    libzcash::SaplingIncomingViewingKey ivk;
    libzcash::SaplingExtendedFullViewingKey extfvk;
    if (!GetIncomingViewingKey(address, ivk) || !GetFullViewingKey(ivk, extfvk)) return false;
    auto sk = GetSpendingKey(extfvk);
    if (!sk) return false;
    sk_out = *sk;
    return true;
}

bool SaplingWallet::GetIncomingViewingKey(const libzcash::SaplingPaymentAddress& address, libzcash::SaplingIncomingViewingKey& ivk_out) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    auto it = m_incoming_viewing_keys.find(address);
    if (it == m_incoming_viewing_keys.end()) return false;
    ivk_out = it->second;
    return true;
}

bool SaplingWallet::GetFullViewingKey(const libzcash::SaplingIncomingViewingKey& ivk, libzcash::SaplingExtendedFullViewingKey& extfvk_out) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    auto it = m_full_viewing_keys.find(ivk);
    if (it == m_full_viewing_keys.end()) return false;
    extfvk_out = it->second;
    return true;
}

void SaplingWallet::GetPaymentAddresses(std::set<libzcash::SaplingPaymentAddress>& addresses) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    for (const auto& [address, ivk] : m_incoming_viewing_keys) {
        addresses.insert(address);
    }
}

std::pair<mapSaplingNoteData_t, SaplingIncomingViewingKeyMap> SaplingWallet::FindMySaplingNotes(const CTransaction& tx) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    if (!tx.HasShieldedPayload()) return {};

    const uint256& hash = tx.GetHash();
    mapSaplingNoteData_t note_data;
    SaplingIncomingViewingKeyMap viewing_keys_to_add;

    for (uint32_t i = 0; i < tx.sapData.vShieldedOutput.size(); ++i) {
        const OutputDescription& output = tx.sapData.vShieldedOutput[i];
        for (const auto& [ivk, extfvk] : m_full_viewing_keys) {
            auto plaintext = libzcash::SaplingNotePlaintext::decrypt(output.encCiphertext, ivk, output.ephemeralKey, output.cmu);
            if (!plaintext) continue;

            Optional<libzcash::SaplingPaymentAddress> address = ivk.address(plaintext->d);
            if (address && m_incoming_viewing_keys.count(*address) == 0) {
                viewing_keys_to_add[*address] = ivk;
            }

            SaplingNoteData nd;
            nd.ivk = ivk;
            nd.amount = static_cast<CAmount>(plaintext->value());
            nd.address = address;
            const auto& memo = plaintext->memo();
            if (memo[0] < 0xF6) nd.memo = memo;
            note_data.emplace(SaplingOutPoint{hash, i}, nd);
            break;
        }
    }

    return {note_data, viewing_keys_to_add};
}

bool SaplingWallet::ApplySaplingData(CWalletTx& wtx, WalletBatch* batch)
{
    AssertLockHeld(m_wallet.cs_wallet);
    bool changed = false;

    if (wtx.tx->HasShieldedPayload()) {
        for (const SpendDescription& spend : wtx.tx->sapData.vShieldedSpend) {
            AddToSaplingSpends(spend.nullifier, wtx.GetHash());
        }
    }

    auto [note_data, viewing_keys_to_add] = FindMySaplingNotes(*wtx.tx);
    for (const auto& [address, ivk] : viewing_keys_to_add) {
        changed |= AddPaymentAddress(address, ivk, batch);
    }
    for (const auto& [op, nd] : note_data) {
        auto it = wtx.mapSaplingNoteData.find(op);
        if (it == wtx.mapSaplingNoteData.end() || it->second != nd) {
            wtx.mapSaplingNoteData[op] = nd;
            changed = true;
        }
    }
    return changed;
}

void SaplingWallet::RescanWalletTransactions()
{
    AssertLockHeld(m_wallet.cs_wallet);
    const auto start{SteadyClock::now()};
    m_sapling_spends.clear();
    m_nullifiers_to_notes.clear();
    WalletBatch batch(m_wallet.GetDatabase());
    size_t shielded_wallet_txs = 0;
    for (auto& [txid, wtx] : m_wallet.mapWallet) {
        if (wtx.tx->HasShieldedPayload()) ++shielded_wallet_txs;
        wtx.mapSaplingNoteData.clear();
        ApplySaplingData(wtx, &batch);
    }

    if (!HasSaplingNotes()) {
        m_wallet.WalletLogPrintf("Sapling wallet rescan found no Sapling notes in %u wallet txs (%u shielded); skipped witness rebuild in %dms\n",
            static_cast<unsigned int>(m_wallet.mapWallet.size()),
            static_cast<unsigned int>(shielded_wallet_txs),
            Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
        return;
    }

    std::string error;
    if (!RebuildWitnesses(&error) && !error.empty()) {
        m_wallet.WalletLogPrintf("Sapling witness rebuild warning: %s\n", error);
    }
    m_wallet.WalletLogPrintf("Sapling wallet rescan completed in %dms\n",
        Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
}

bool SaplingWallet::HasSaplingNotes() const
{
    AssertLockHeld(m_wallet.cs_wallet);
    for (const auto& [txid, wtx] : m_wallet.mapWallet) {
        for (const auto& [op, nd] : wtx.mapSaplingNoteData) {
            if (nd.IsMyNote()) return true;
        }
    }
    return false;
}

void SaplingWallet::ClearWitnesses()
{
    AssertLockHeld(m_wallet.cs_wallet);
    m_nullifiers_to_notes.clear();
    for (auto& [txid, wtx] : m_wallet.mapWallet) {
        for (auto& [op, nd] : wtx.mapSaplingNoteData) {
            nd.witnesses.clear();
            nd.witnessHeight = -1;
            nd.nullifier = nullopt;
        }
    }
}

bool SaplingWallet::RebuildWitnesses(std::string* error)
{
    AssertLockHeld(m_wallet.cs_wallet);
    if (!HasSaplingNotes()) return true;
    ClearWitnesses();

    if (!m_wallet.HaveChain()) return true;
    const auto tip_height = m_wallet.chain().getHeight();
    if (!tip_height) return true;

    const int shield_height = Params().GetConsensus().nSMTShieldHeight;
    if (shield_height < 0 || shield_height > *tip_height) return true;

    SaplingMerkleTree tree;
    for (int height = shield_height; height <= *tip_height; ++height) {
        const uint256 block_hash = m_wallet.chain().getBlockHash(height);
        CBlock block;
        if (!m_wallet.chain().findBlock(block_hash, FoundBlock().data(block)) || block.IsNull()) {
            if (error) *error = strprintf("unable to read block %d while rebuilding Sapling witnesses", height);
            return false;
        }

        for (const auto& tx : block.vtx) {
            if (!tx->HasShieldedPayload()) continue;
            const uint256 txid = tx->GetHash();

            for (const SpendDescription& spend : tx->sapData.vShieldedSpend) {
                AddToSaplingSpends(spend.nullifier, txid);
            }

            auto wallet_tx_it = m_wallet.mapWallet.find(txid);
            for (uint32_t i = 0; i < tx->sapData.vShieldedOutput.size(); ++i) {
                const uint256& cmu = tx->sapData.vShieldedOutput[i].cmu;
                AppendCommitmentToWalletWitnesses(m_wallet, cmu);
                tree.append(cmu);

                if (wallet_tx_it == m_wallet.mapWallet.end()) continue;
                CWalletTx& wtx = wallet_tx_it->second;
                auto nd_it = wtx.mapSaplingNoteData.find(SaplingOutPoint{txid, i});
                if (nd_it == wtx.mapSaplingNoteData.end() || !nd_it->second.IsMyNote()) continue;
                nd_it->second.witnesses.push_front(tree.witness());
                nd_it->second.witnessHeight = height;
            }
        }
    }

    for (auto& [txid, wtx] : m_wallet.mapWallet) {
        for (auto& [op, nd] : wtx.mapSaplingNoteData) {
            if (!nd.IsMyNote() || nd.witnesses.empty() || !nd.ivk) continue;
            auto note = DecryptNote(*wtx.tx, op, nd);
            if (!note) continue;
            libzcash::SaplingExtendedFullViewingKey extfvk;
            if (!GetFullViewingKey(*nd.ivk, extfvk)) continue;
            auto nullifier = note->nullifier(extfvk.fvk, nd.witnesses.front().position());
            if (!nullifier) continue;
            nd.nullifier = *nullifier;
            m_nullifiers_to_notes[*nullifier] = op;
        }
    }

    return true;
}

void SaplingWallet::AddToSaplingSpends(const uint256& nullifier, const uint256& wtxid)
{
    AssertLockHeld(m_wallet.cs_wallet);
    m_sapling_spends.emplace(nullifier, wtxid);
}

bool SaplingWallet::IsSaplingSpendFromMe(const CTransaction& tx) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    if (!tx.HasShieldedPayload()) return false;
    for (const SpendDescription& spend : tx.sapData.vShieldedSpend) {
        if (m_nullifiers_to_notes.count(spend.nullifier) != 0) return true;
    }
    return false;
}

bool SaplingWallet::IsSaplingSpent(const SaplingOutPoint& op) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    for (const auto& [nullifier, note_op] : m_nullifiers_to_notes) {
        if (note_op == op) return IsSaplingSpent(nullifier);
    }
    return false;
}

bool SaplingWallet::IsSaplingSpent(const uint256& nullifier) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    auto range = m_sapling_spends.equal_range(nullifier);
    for (auto it = range.first; it != range.second; ++it) {
        const auto wtx_it = m_wallet.mapWallet.find(it->second);
        if (wtx_it == m_wallet.mapWallet.end()) continue;
        const CWalletTx& wtx = wtx_it->second;
        if (!wtx.isAbandoned() && !wtx.isConflicted() && m_wallet.GetTxDepthInMainChain(wtx) >= 0) {
            return true;
        }
    }
    return false;
}

void SaplingWallet::GetFilteredNotes(std::vector<SaplingNoteEntry>& notes,
                                     const Optional<libzcash::SaplingPaymentAddress>& address,
                                     int min_depth,
                                     bool ignore_spent,
                                     bool require_spending_key)
{
    AssertLockHeld(m_wallet.cs_wallet);
    std::string error;
    if (!RebuildWitnesses(&error) && !error.empty()) {
        m_wallet.WalletLogPrintf("Sapling witness rebuild warning: %s\n", error);
    }

    for (const auto& [txid, wtx] : m_wallet.mapWallet) {
        const int depth = m_wallet.GetTxDepthInMainChain(wtx);
        if (depth < min_depth) continue;

        for (const auto& [op, nd] : wtx.mapSaplingNoteData) {
            if (!nd.IsMyNote() || !nd.address) continue;
            if (address && !(*nd.address == *address)) continue;
            if (ignore_spent && nd.nullifier && IsSaplingSpent(*nd.nullifier)) continue;
            if (require_spending_key && !HaveSpendingKeyForPaymentAddress(*nd.address)) continue;

            auto note = DecryptNote(*wtx.tx, op, nd);
            if (!note) continue;
            std::array<unsigned char, ZC_MEMO_SIZE> memo = {{0}};
            if (nd.memo) memo = *nd.memo;
            notes.emplace_back(op, *nd.address, *note, memo, depth);
        }
    }
}

void SaplingWallet::GetSpendableNotes(std::vector<SaplingSpendableNoteEntry>& notes,
                                      const Optional<libzcash::SaplingPaymentAddress>& address,
                                      int min_depth)
{
    AssertLockHeld(m_wallet.cs_wallet);
    std::string error;
    if (!RebuildWitnesses(&error) && !error.empty()) {
        m_wallet.WalletLogPrintf("Sapling witness rebuild warning: %s\n", error);
    }

    for (const auto& [txid, wtx] : m_wallet.mapWallet) {
        const int depth = m_wallet.GetTxDepthInMainChain(wtx);
        if (depth < min_depth) continue;

        for (const auto& [op, nd] : wtx.mapSaplingNoteData) {
            if (!nd.IsMyNote() || !nd.address || nd.witnesses.empty()) continue;
            if (address && !(*nd.address == *address)) continue;
            if (nd.nullifier && IsSaplingSpent(*nd.nullifier)) continue;
            if (!HaveSpendingKeyForPaymentAddress(*nd.address)) continue;

            auto note = DecryptNote(*wtx.tx, op, nd);
            if (!note) continue;
            std::array<unsigned char, ZC_MEMO_SIZE> memo = {{0}};
            if (nd.memo) memo = *nd.memo;
            notes.emplace_back(op, *nd.address, *note, memo, depth, nd.witnesses.front());
        }
    }
}

CAmount SaplingWallet::GetBalance(const Optional<libzcash::SaplingPaymentAddress>& address, int min_depth, bool ignore_unspendable)
{
    AssertLockHeld(m_wallet.cs_wallet);
    std::vector<SaplingNoteEntry> notes;
    GetFilteredNotes(notes, address, min_depth, /*ignore_spent=*/true, /*require_spending_key=*/ignore_unspendable);
    CAmount balance = 0;
    for (const auto& note : notes) {
        balance += static_cast<CAmount>(note.note.value());
    }
    return balance;
}

bool SaplingWallet::AddPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                                      const libzcash::SaplingIncomingViewingKey& ivk,
                                      WalletBatch* batch)
{
    AssertLockHeld(m_wallet.cs_wallet);
    m_incoming_viewing_keys.insert_or_assign(address, ivk);
    WalletBatch local_batch(m_wallet.GetDatabase());
    WalletBatch& db = batch ? *batch : local_batch;
    return db.WriteSaplingPaymentAddress(address, ivk);
}

Optional<libzcash::SaplingNote> SaplingWallet::DecryptNote(const CTransaction& tx, const SaplingOutPoint& op, const SaplingNoteData& nd) const
{
    AssertLockHeld(m_wallet.cs_wallet);
    if (!nd.ivk || op.n >= tx.sapData.vShieldedOutput.size()) return nullopt;
    const OutputDescription& output = tx.sapData.vShieldedOutput[op.n];
    auto plaintext = libzcash::SaplingNotePlaintext::decrypt(output.encCiphertext, *nd.ivk, output.ephemeralKey, output.cmu);
    if (!plaintext) return nullopt;
    return plaintext->note(*nd.ivk);
}

} // namespace wallet
