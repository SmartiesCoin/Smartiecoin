// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020-2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_SAPLING_SAPLING_TRANSACTION_H
#define PIVX_SAPLING_SAPLING_TRANSACTION_H

#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "serialize.h"
#include "streams.h"
#include "uint256.h"

#include "sapling/noteencryption.h"
#include "sapling/sapling.h"

#include <algorithm>
#include <boost/variant.hpp>
#include <limits>
#include <string>

// transaction.h comment: spending taddr output requires CTxIn >= 148 bytes and typical taddr txout is 34 bytes
#define CTXIN_SPEND_DUST_SIZE   149
#define CTXOUT_REGULAR_SIZE     34

// These constants are defined in the protocol § 7.1:
// https://zips.z.cash/protocol/protocol.pdf#txnencoding
#define OUTPUTDESCRIPTION_SIZE  948
#define SPENDDESCRIPTION_SIZE   384
#define BINDINGSIG_SIZE          64

namespace libzcash {
    static constexpr size_t GROTH_PROOF_SIZE = (
            48 + // π_A
            96 + // π_B
            48); // π_C

    typedef std::array<unsigned char, GROTH_PROOF_SIZE> GrothProof;
}

/** Identifies a Sapling output by transaction hash and shielded output index. */
class SaplingOutPoint
{
public:
    uint256 hash;
    uint32_t n;

    static constexpr uint32_t NULL_INDEX = std::numeric_limits<uint32_t>::max();

    SaplingOutPoint() : n(NULL_INDEX) {}
    SaplingOutPoint(const uint256& hashIn, uint32_t nIn) : hash(hashIn), n(nIn) {}

    SERIALIZE_METHODS(SaplingOutPoint, obj) { READWRITE(obj.hash, obj.n); }

    void SetNull() { hash.SetNull(); n = NULL_INDEX; }
    bool IsNull() const { return hash.IsNull() && n == NULL_INDEX; }

    std::string ToString() const { return hash.ToString() + "-" + std::to_string(n); }

    friend bool operator<(const SaplingOutPoint& a, const SaplingOutPoint& b)
    {
        int cmp = a.hash.Compare(b.hash);
        return cmp < 0 || (cmp == 0 && a.n < b.n);
    }

    friend bool operator==(const SaplingOutPoint& a, const SaplingOutPoint& b)
    {
        return a.hash == b.hash && a.n == b.n;
    }

    friend bool operator!=(const SaplingOutPoint& a, const SaplingOutPoint& b)
    {
        return !(a == b);
    }
};

/**
 * A shielded input to a transaction. It contains data that describes a Spend transfer.
 */
class SpendDescription
{
public:
    typedef std::array<unsigned char, 64> spend_auth_sig_t;

    uint256 cv{};                          //!< A value commitment to the value of the input note.
    uint256 anchor{};                      //!< A Merkle root of the Sapling note commitment tree at some block height in the past.
    uint256 nullifier{};                   //!< The nullifier of the input note.
    uint256 rk{};                          //!< The randomized public key for spendAuthSig.
    libzcash::GrothProof zkproof = {{0}};  //!< A zero-knowledge proof using the spend circuit.
    spend_auth_sig_t spendAuthSig = {{0}}; //!< A signature authorizing this spend.

    SpendDescription() {}

    SERIALIZE_METHODS(SpendDescription, obj)
    {
        READWRITE(obj.cv, obj.anchor, obj.nullifier, obj.rk);
        READWRITE(Span{obj.zkproof}, Span{obj.spendAuthSig});
    }

    friend bool operator==(const SpendDescription& a, const SpendDescription& b)
    {
        return (
                a.cv == b.cv &&
                a.anchor == b.anchor &&
                a.nullifier == b.nullifier &&
                a.rk == b.rk &&
                a.zkproof == b.zkproof &&
                a.spendAuthSig == b.spendAuthSig
        );
    }

    friend bool operator!=(const SpendDescription& a, const SpendDescription& b)
    {
        return !(a == b);
    }
};

/**
 * A shielded output to a transaction. It contains data that describes an Output transfer.
 */
class OutputDescription
{
public:
    uint256 cv{};                                         //!< A value commitment to the value of the output note.
    uint256 cmu{};                                        //!< The u-coordinate of the note commitment for the output note.
    uint256 ephemeralKey{};                               //!< A Jubjub public key.
    libzcash::SaplingEncCiphertext encCiphertext = {{0}}; //!< A ciphertext component for the encrypted output note.
    libzcash::SaplingOutCiphertext outCiphertext = {{0}}; //!< A ciphertext component for the encrypted output note.
    libzcash::GrothProof zkproof = {{0}};                 //!< A zero-knowledge proof using the output circuit.

    OutputDescription() {}

    SERIALIZE_METHODS(OutputDescription, obj)
    {
        READWRITE(obj.cv, obj.cmu, obj.ephemeralKey);
        READWRITE(Span{obj.encCiphertext}, Span{obj.outCiphertext}, Span{obj.zkproof});
    }

    friend bool operator==(const OutputDescription& a, const OutputDescription& b)
    {
        return (
                a.cv == b.cv &&
                a.cmu == b.cmu &&
                a.ephemeralKey == b.ephemeralKey &&
                a.encCiphertext == b.encCiphertext &&
                a.outCiphertext == b.outCiphertext &&
                a.zkproof == b.zkproof
        );
    }

    friend bool operator!=(const OutputDescription& a, const OutputDescription& b)
    {
        return !(a == b);
    }
};

class SaplingTxData
{
public:
    typedef std::array<unsigned char, BINDINGSIG_SIZE> binding_sig_t;

    CAmount valueBalance{0};
    std::vector<SpendDescription> vShieldedSpend;
    std::vector<OutputDescription> vShieldedOutput;
    binding_sig_t bindingSig = {{0}};

    SERIALIZE_METHODS(SaplingTxData, obj)
    {
        READWRITE(obj.valueBalance, obj.vShieldedSpend, obj.vShieldedOutput);
        READWRITE(Span{obj.bindingSig});
    }

    SaplingTxData() = default;
    SaplingTxData(const SaplingTxData&) = default;
    SaplingTxData& operator=(const SaplingTxData&) = default;
    SaplingTxData(SaplingTxData&&) = default;
    SaplingTxData& operator=(SaplingTxData&&) = default;

    bool hasBindingSig() const
    {
        return std::any_of(bindingSig.begin(), bindingSig.end(),
                          [](const unsigned char& c){ return c != 0; });
    }

    bool IsEmpty() const
    {
        return valueBalance == 0 &&
               vShieldedSpend.empty() &&
               vShieldedOutput.empty() &&
               !hasBindingSig();
    }
};


#endif // PIVX_SAPLING_SAPLING_TRANSACTION_H
