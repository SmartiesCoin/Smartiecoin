// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SAPLING_WALLET_H
#define BITCOIN_WALLET_SAPLING_WALLET_H

#include <sapling/zip32.h>
#include <sync.h>
#include <wallet/crypter.h>
#include <wallet/sapling_types.h>
#include <wallet/walletdb.h>

#include <map>
#include <set>
#include <string>
#include <vector>

class CTransaction;

namespace wallet {

class CWallet;
class CWalletTx;

using SaplingIncomingViewingKeyMap = std::map<libzcash::SaplingPaymentAddress, libzcash::SaplingIncomingViewingKey>;

class SaplingWallet
{
public:
    explicit SaplingWallet(CWallet& wallet) : m_wallet(wallet) {}

    libzcash::SaplingPaymentAddress GenerateNewAddress(const std::string& label = "");

    bool AddSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk,
                        int64_t create_time,
                        WalletBatch* batch = nullptr);
    bool LoadSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk);
    bool LoadCryptedSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk,
                                const std::vector<unsigned char>& crypted_secret);
    bool LoadKeyMetadata(const libzcash::SaplingIncomingViewingKey& ivk, const CKeyMetadata& meta);
    bool LoadPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                            const libzcash::SaplingIncomingViewingKey& ivk);

    bool EncryptKeys(const CKeyingMaterial& master_key, WalletBatch& batch);
    bool CheckDecryptionKey(const CKeyingMaterial& master_key) const;

    bool HaveSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk) const;
    bool HaveSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address) const;
    bool GetSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                                         libzcash::SaplingExtendedSpendingKey& sk_out) const;
    bool GetIncomingViewingKey(const libzcash::SaplingPaymentAddress& address,
                               libzcash::SaplingIncomingViewingKey& ivk_out) const;
    bool GetFullViewingKey(const libzcash::SaplingIncomingViewingKey& ivk,
                           libzcash::SaplingExtendedFullViewingKey& extfvk_out) const;
    void GetPaymentAddresses(std::set<libzcash::SaplingPaymentAddress>& addresses) const;

    std::pair<mapSaplingNoteData_t, SaplingIncomingViewingKeyMap> FindMySaplingNotes(const CTransaction& tx) const;
    bool ApplySaplingData(CWalletTx& wtx, WalletBatch* batch = nullptr);
    void RescanWalletTransactions();
    bool RebuildWitnesses(std::string* error = nullptr);

    void AddToSaplingSpends(const uint256& nullifier, const uint256& wtxid);
    bool IsSaplingSpendFromMe(const CTransaction& tx) const;
    bool IsSaplingSpent(const SaplingOutPoint& op) const;
    bool IsSaplingSpent(const uint256& nullifier) const;

    void GetFilteredNotes(std::vector<SaplingNoteEntry>& notes,
                          const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                          int min_depth = 1,
                          bool ignore_spent = true,
                          bool require_spending_key = true);
    void GetSpendableNotes(std::vector<SaplingSpendableNoteEntry>& notes,
                           const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                           int min_depth = 1);
    CAmount GetBalance(const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                       int min_depth = 1,
                       bool ignore_unspendable = true);

private:
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
