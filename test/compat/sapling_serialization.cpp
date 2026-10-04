// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Serialization-boundary fixtures; no cryptographic validity is claimed.
// The expected output is captured from the pre-migration Boost implementation.
#include <hash.h>
#include <primitives/transaction.h>
#include <sapling/incrementalmerkletree.h>
#include <sapling/sapling_transaction.h>
#include <sapling/zip32.h>
#include <streams.h>

#include <cassert>
#include <iomanip>
#include <iostream>

using namespace libzcash;

template <typename T> std::vector<std::byte> Bytes(const T& value)
{
    CDataStream stream(SER_DISK, 0);
    stream << value;
    return {stream.begin(), stream.end()};
}

template <typename T> T Read(const std::vector<std::byte>& bytes)
{
    CDataStream stream(bytes, SER_DISK, 0);
    T value{};
    stream >> value;
    assert(stream.empty());
    return value;
}

template <typename T> void Check(const char* name, const T& value)
{
    const auto bytes = Bytes(value);
    assert(Bytes(Read<T>(bytes)) == bytes);
    for (size_t length = 0; length < bytes.size(); ++length) {
        const std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + length);
        bool threw{false};
        try { (void)Read<T>(truncated); } catch (const std::ios_base::failure&) { threw = true; }
        assert(threw);
    }
    std::cout << name << ' ';
    for (auto byte : bytes) std::cout << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    std::cout << std::dec << '\n';
}

int main()
{
    // Assemble serialized private tree fields using the real serializers, then
    // exercise the actual Sapling tree/witness Unserialize (including wfcheck
    // and cursor-depth reconstruction) and Serialize. No fake hash function.
    const PedersenHash leaf{uint256::ONE};
    CDataStream fields(SER_DISK, 0);
    fields << Optional<PedersenHash>{leaf} << Optional<PedersenHash>{}
           << std::vector<Optional<PedersenHash>>{nullopt, leaf};
    SaplingMerkleTree tree;
    fields >> tree;
    assert(tree.size() == 5);
    Check("tree", tree);
    Check("empty-tree", SaplingMerkleTree{});

    CDataStream cursor_fields(SER_DISK, 0);
    cursor_fields << Optional<PedersenHash>{leaf} << Optional<PedersenHash>{}
                  << std::vector<Optional<PedersenHash>>{};
    SaplingMerkleTree cursor;
    cursor_fields >> cursor;
    CDataStream witness_fields(SER_DISK, 0);
    witness_fields << tree << std::vector<PedersenHash>{leaf}
                   << Optional<SaplingMerkleTree>{cursor};
    SaplingWitness witness;
    witness_fields >> witness;
    assert(witness.position() == 4 && witness.element() == leaf);
    Check("witness-with-cursor", witness);
    Check("witness-list", std::vector<SaplingWitness>{witness, witness});

    CDataStream invalid_fields(SER_DISK, 0);
    invalid_fields << Optional<PedersenHash>{} << Optional<PedersenHash>{leaf}
                   << std::vector<Optional<PedersenHash>>{};
    bool rejected{false};
    try { invalid_fields >> tree; } catch (const std::ios_base::failure&) { rejected = true; }
    assert(rejected);

    SaplingTxData tx;
    tx.valueBalance = -123456789;
    SpendDescription spend;
    spend.cv = uint256::ONE;
    spend.anchor.begin()[7] = 23;
    spend.nullifier.begin()[13] = 42;
    spend.rk.begin()[31] = 99;
    spend.zkproof.fill(0x5a);
    spend.spendAuthSig.fill(0xa5);
    tx.vShieldedSpend.push_back(spend);
    OutputDescription output;
    output.cv = uint256::ONE;
    output.cmu.begin()[11] = 17;
    output.ephemeralKey.begin()[29] = 31;
    output.encCiphertext.fill(0x12);
    output.outCiphertext.fill(0x34);
    output.zkproof.fill(0x56);
    tx.vShieldedOutput.push_back(output);
    tx.bindingSig.fill(0x78);
    Check("sapling-transaction", tx);
    Check("optional-sapling-transaction", Optional<SaplingTxData>{tx});
    CMutableTransaction transaction;
    transaction.nVersion = CTransaction::SHIELDED_VERSION;
    transaction.sapData = tx;
    transaction.nLockTime = 12345;
    Check("shielded-mutable-transaction", transaction);
    transaction.nVersion = CTransaction::SPECIAL_VERSION;
    Check("legacy-mutable-transaction-no-sapling", transaction);
    Check("spend-vector", std::vector<SpendDescription>{spend, spend});
    Check("output-vector", std::vector<OutputDescription>{output, output});

    // These key/value shapes and tags are those in WalletBatch's three Sapling
    // writers. The runner verifies the tag literals against walletdb.cpp.
    SaplingIncomingViewingKey ivk{uint256::ONE};
    SaplingPaymentAddress address;
    for (size_t i = 0; i < address.d.size(); ++i) address.d[i] = i + 1;
    address.pk_d = uint256::ONE;
    SaplingExtendedSpendingKey spending{};
    spending.depth = 7;
    spending.childIndex = 0xabcdef01;
    SaplingExtendedFullViewingKey viewing{};
    viewing.depth = spending.depth;
    viewing.childIndex = spending.childIndex;
    Check("wallet-key-key", std::make_pair(std::string{"sapzkey"}, ivk));
    Check("wallet-key-value", spending);
    Check("wallet-address-key", std::make_pair(std::string{"sapzaddr"}, address));
    Check("wallet-address-value", ivk);
    Check("wallet-encrypted-key", std::make_pair(std::string{"csapzkey"}, viewing));
    // Use the same checksum operation as WalletBatch::WriteCryptedSaplingZKey.
    const std::vector<unsigned char> encrypted{1, 2, 3, 4};
    Check("wallet-encrypted-value", std::make_pair(encrypted, Hash(encrypted)));
}
