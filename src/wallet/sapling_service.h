// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SAPLING_SERVICE_H
#define BITCOIN_WALLET_SAPLING_SERVICE_H

#include <sapling/zip32.h>
#include <wallet/crypter.h>
#include <wallet/sapling_types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

class CTransaction;

namespace wallet {
class CWallet;
class CWalletTx;
class WalletBatch;
class CKeyMetadata;

using SaplingIncomingViewingKeyMap = std::map<libzcash::SaplingPaymentAddress, libzcash::SaplingIncomingViewingKey>;

/** Wallet-owned Sapling operations. Callers retain cs_wallet and batch ownership.
 * Construction/destruction must not access the host: its mutex and transaction
 * state are constructed after, and destroyed before, this service.
 */
class SaplingService
{
public:
    virtual ~SaplingService() = default;

    virtual libzcash::SaplingPaymentAddress GenerateNewAddress(const std::string& label = "") = 0;
    virtual bool AddSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk,
                        int64_t create_time,
                        WalletBatch* batch = nullptr) = 0;
    virtual bool LoadSpendingKey(const libzcash::SaplingExtendedSpendingKey& sk) = 0;
    virtual bool LoadCryptedSpendingKey(const libzcash::SaplingExtendedFullViewingKey& extfvk,
                                const std::vector<unsigned char>& crypted_secret) = 0;
    virtual bool LoadKeyMetadata(const libzcash::SaplingIncomingViewingKey& ivk, const CKeyMetadata& meta) = 0;
    virtual bool LoadPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                            const libzcash::SaplingIncomingViewingKey& ivk) = 0;
    virtual bool EncryptKeys(const CKeyingMaterial& master_key, WalletBatch& batch) = 0;
    virtual bool CheckDecryptionKey(const CKeyingMaterial& master_key) const = 0;
    virtual bool HaveSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address) const = 0;
    virtual bool GetSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress& address,
                                         libzcash::SaplingExtendedSpendingKey& sk_out) const = 0;
    virtual bool GetIncomingViewingKey(const libzcash::SaplingPaymentAddress& address,
                               libzcash::SaplingIncomingViewingKey& ivk_out) const = 0;
    virtual bool GetFullViewingKey(const libzcash::SaplingIncomingViewingKey& ivk,
                           libzcash::SaplingExtendedFullViewingKey& extfvk_out) const = 0;
    virtual void GetPaymentAddresses(std::set<libzcash::SaplingPaymentAddress>& addresses) const = 0;
    virtual std::pair<mapSaplingNoteData_t, SaplingIncomingViewingKeyMap> FindMySaplingNotes(const CTransaction& tx) const = 0;
    // Returns whether note/address data changed; throws std::runtime_error on
    // transaction/address storage failure, before publishing notes or spends. Do not swallow
    // this error in notifications: SyncTransaction follows the generic wallet
    // write fail-stop policy (an uncaught asynchronous error is terminal).
    //
    // A supplied active batch remains caller-owned and rollback-only after write
    // failure. In that case wtx MUST be a caller-owned provisional transaction,
    // not a live mapWallet entry: notes are updated immediately in that temporary.
    // Publish its note data only after successful owner commit; discard it on
    // failure/abort/destruction. Addresses and global spends publish on commit.
    // No callback retains wtx. Completion and service destruction are serialized
    // under cs_wallet; callbacks for a destroyed service are discarded. This is
    // a caller-staging contract, NOT automatic rollback of the supplied wtx.
    // Production AddToWallet and RescanWalletTransactions use fresh non-active
    // batches, so their note publication does not depend on joined completion.
    virtual bool ApplySaplingData(CWalletTx& wtx, WalletBatch* batch = nullptr) = 0;
    // On exception restores all prior note data (including witnesses) and both
    // auxiliary indexes, then rethrows. Already committed address discoveries
    // remain valid. Normal success replaces stale data and rebuilds witnesses;
    // the existing nonthrowing witness-rebuild warning policy is unchanged.
    virtual void RescanWalletTransactions() = 0;
    virtual void AddToSaplingSpends(const uint256& nullifier, const uint256& wtxid) = 0;
    virtual bool IsSaplingSpendFromMe(const CTransaction& tx) const = 0;
    virtual bool IsSaplingSpent(const SaplingOutPoint& op) const = 0;
    virtual void GetFilteredNotes(std::vector<SaplingNoteEntry>& notes,
                          const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                          int min_depth = 1,
                          bool ignore_spent = true,
                          bool require_spending_key = true) = 0;
    virtual void GetSpendableNotes(std::vector<SaplingSpendableNoteEntry>& notes,
                           const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                           int min_depth = 1) = 0;
    virtual CAmount GetBalance(const Optional<libzcash::SaplingPaymentAddress>& address = nullopt,
                       int min_depth = 1,
                       bool ignore_unspendable = true) = 0;
};

/** Construct the existing implementation; no database I/O or host callbacks. */
std::unique_ptr<SaplingService> MakeSaplingService(CWallet& wallet);

} // namespace wallet

#endif // BITCOIN_WALLET_SAPLING_SERVICE_H
