// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SAPLING_WALLET_H
#define BITCOIN_WALLET_SAPLING_WALLET_H

#include <sapling/zip32.h>
#include <sync.h>
#include <wallet/crypter.h>
#include <wallet/sapling_service.h>
#include <wallet/walletdb.h>

#include <map>
#include <set>
#include <string>
#include <vector>

class CTransaction;

namespace wallet {

class CWallet;
class CWalletTx;

class SaplingWallet final : public SaplingService
{
public:
    explicit SaplingWallet(CWallet& wallet) : m_wallet(wallet) {}
    ~SaplingWallet() override { m_lifetime.reset(); }
    SaplingWallet(const SaplingWallet&) = delete;
    SaplingWallet& operator=(const SaplingWallet&) = delete;
    SaplingWallet(SaplingWallet&&) = delete;
    SaplingWallet& operator=(SaplingWallet&&) = delete;

    libzcash::SaplingPaymentAddress GenerateNewAddress(const std::string& label = "") override;

    bool AddSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk,
                        int64_t create_time,
                        WalletBatch* batch = nullptr) override;
    bool LoadSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk) override;
    bool LoadCryptedSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk,
                                const std::vector<unsigned char>& crypted_secret) override;
    bool LoadKeyMetadata(const libzcash::SaplingIncomingViewingKey& ivk, const CKeyMetadata& meta) override;
    bool LoadPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                            const libzcash::SaplingIncomingViewingKey& ivk) override;

    bool EncryptKeys(const CKeyingMaterial& master_key, WalletBatch& batch) override;
    bool CheckDecryptionKey(const CKeyingMaterial& master_key) const override;

    bool HaveSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk) const;
    bool HaveSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address) const override;
    bool GetSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                                         libzcash::SaplingExtendedSpendingKey& sk_out) const override;
    bool GetIncomingViewingKey(const libzcash::SaplingPaymentAddress& address,
                               libzcash::SaplingIncomingViewingKey& ivk_out) const override;
    bool GetFullViewingKey(const libzcash::SaplingIncomingViewingKey& ivk,
                           libzcash::SaplingExtendedFullViewingKey& extfvk_out) const override;
    void GetPaymentAddresses(std::set<libzcash::SaplingPaymentAddress>& addresses) const override;

    std::pair<mapSaplingNoteData_t, SaplingIncomingViewingKeyMap> FindMySaplingNotes(const CTransaction& tx) const override;
    bool ApplySaplingData(CWalletTx& wtx, WalletBatch* batch = nullptr) override;
    void RescanWalletTransactions() override;
    bool RebuildWitnesses(std::string* error = nullptr);

    void AddToSaplingSpends(const uint256& nullifier, const uint256& wtxid) override;
    bool IsSaplingSpendFromMe(const CTransaction& tx) const override;
    bool IsSaplingSpent(const SaplingOutPoint& op) const override;
    bool IsSaplingSpent(const uint256& nullifier) const;

    void GetFilteredNotes(std::vector<SaplingNoteEntry>& notes,
                          const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                          int min_depth = 1,
                          bool ignore_spent = true,
                          bool require_spending_key = true) override;
    void GetSpendableNotes(std::vector<SaplingSpendableNoteEntry>& notes,
                           const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                           int min_depth = 1) override;
    CAmount GetBalance(const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                       int min_depth = 1,
                       bool ignore_unspendable = true) override;

private:
    friend struct SaplingWalletTestAccess;
    bool AddPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                           const libzcash::SaplingIncomingViewingKey& ivk,
                           WalletBatch* batch);
    Optional<libzcash::SaplingExtendedSpendingKey> GetSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk) const;
    Optional<libzcash::SaplingNote> DecryptNote(const CTransaction& tx,
                                                const SaplingOutPoint& op,
                                                const SaplingNoteData& nd) const;
    bool HasSaplingNotes() const;
    void ClearWitnesses();

    CWallet& m_wallet;
    // Completion actions are synchronous under cs_wallet. An expired token
    // discards publication if the service was destroyed before the batch.
    std::shared_ptr<const int> m_lifetime{std::make_shared<const int>(0)};

    std::map<libzcash::SaplingIncomingViewingKey, libzcash::SaplingExtendedFullViewingKey> m_full_viewing_keys;
    std::map<libzcash::SaplingPaymentAddress, libzcash::SaplingIncomingViewingKey> m_incoming_viewing_keys;
    std::map<libzcash::SaplingExtendedFullViewingKey, libzcash::SaplingExtendedSpendingKey> m_spending_keys;
    std::map<libzcash::SaplingExtendedFullViewingKey, std::vector<unsigned char>> m_crypted_spending_keys;
    std::map<libzcash::SaplingIncomingViewingKey, CKeyMetadata> m_key_metadata;
    std::multimap<uint256, uint256> m_sapling_spends;
    std::map<uint256, SaplingOutPoint> m_nullifiers_to_notes;
};

} // namespace wallet

#endif // BITCOIN_WALLET_SAPLING_WALLET_H
