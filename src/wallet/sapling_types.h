// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SAPLING_TYPES_H
#define BITCOIN_WALLET_SAPLING_TYPES_H

#include <optional.h>
#include <sapling/address.h>
#include <sapling/incrementalmerkletree.h>
#include <sapling/note.h>
#include <sapling/sapling.h>
#include <sapling/sapling_transaction.h>
#include <uint256.h>

#include <array>
#include <list>
#include <map>

namespace wallet {

/** Sapling note, its location in a transaction, and number of confirmations. */
struct SaplingNoteEntry
{
    SaplingNoteEntry(const SaplingOutPoint& op_in,
                     const libzcash::SaplingPaymentAddress& address_in,
                     const libzcash::SaplingNote& note_in,
                     const std::array<unsigned char, ZC_MEMO_SIZE>& memo_in,
                     int confirmations_in) :
        op(op_in),
        address(address_in),
        note(note_in),
        memo(memo_in),
        confirmations(confirmations_in)
    {
    }

    SaplingOutPoint op;
    libzcash::SaplingPaymentAddress address;
    libzcash::SaplingNote note;
    std::array<unsigned char, ZC_MEMO_SIZE> memo;
    int confirmations;
};

/** Wallet-owned Sapling note with the witness data required for spending. */
struct SaplingSpendableNoteEntry : public SaplingNoteEntry
{
    SaplingSpendableNoteEntry(const SaplingOutPoint& op_in,
                              const libzcash::SaplingPaymentAddress& address_in,
                              const libzcash::SaplingNote& note_in,
                              const std::array<unsigned char, ZC_MEMO_SIZE>& memo_in,
                              int confirmations_in,
                              const SaplingWitness& witness_in) :
        SaplingNoteEntry(op_in, address_in, note_in, memo_in, confirmations_in),
        witness(witness_in),
        anchor(witness_in.root())
    {
    }

    SaplingWitness witness;
    uint256 anchor;
};

/** Wallet-owned Sapling note metadata. Stored in memory and rebuilt from wallet transactions. */
class SaplingNoteData
{
public:
    SaplingNoteData() = default;
    explicit SaplingNoteData(const libzcash::SaplingIncomingViewingKey& ivk_in) : ivk{ivk_in} {}

    std::list<SaplingWitness> witnesses;
    Optional<libzcash::SaplingIncomingViewingKey> ivk{nullopt};
    Optional<CAmount> amount{nullopt};
    Optional<libzcash::SaplingPaymentAddress> address{nullopt};
    Optional<std::array<unsigned char, ZC_MEMO_SIZE>> memo{nullopt};
    int witnessHeight{-1};
    Optional<uint256> nullifier{nullopt};

    bool IsMyNote() const { return ivk != nullopt; }

    friend bool operator==(const SaplingNoteData& a, const SaplingNoteData& b)
    {
        return a.ivk == b.ivk &&
               a.nullifier == b.nullifier &&
               a.witnessHeight == b.witnessHeight &&
               a.amount == b.amount &&
               a.address == b.address &&
               a.memo == b.memo;
    }

    friend bool operator!=(const SaplingNoteData& a, const SaplingNoteData& b)
    {
        return !(a == b);
    }
};

using mapSaplingNoteData_t = std::map<SaplingOutPoint, SaplingNoteData>;

} // namespace wallet

#endif // BITCOIN_WALLET_SAPLING_TYPES_H
