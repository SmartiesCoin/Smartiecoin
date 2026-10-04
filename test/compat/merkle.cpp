// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sapling/incrementalmerkletree.h>
#include <streams.h>

#include <cassert>
#include <iomanip>
#include <iostream>

// Link the project's actual Rust archive: never replace the Pedersen FFI.
// Small field-element leaves are serialization fixtures, not spendable notes.
template <typename T> void Check(const char* name, const T& value)
{
    CDataStream encoded(SER_DISK, 0);
    encoded << value;
    const auto original = std::vector<std::byte>(encoded.begin(), encoded.end());
    T decoded;
    encoded >> decoded;
    assert(encoded.empty());
    assert(decoded.root() == value.root());
    CDataStream rewritten(SER_DISK, 0);
    rewritten << decoded;
    assert(std::vector<std::byte>(rewritten.begin(), rewritten.end()) == original);
    std::cout << name << ' ';
    for (auto byte : original) std::cout << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    std::cout << ' ';
    for (auto byte : value.root()) std::cout << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    std::cout << std::dec << '\n';
}

int main()
{
    SaplingMerkleTree tree;
    tree.append(libzcash::PedersenHash{uint256::ONE});
    auto witness = tree.witness();
    for (unsigned char i = 2; i <= 16; ++i) {
        uint256 leaf;
        leaf.begin()[0] = i;
        tree.append(libzcash::PedersenHash{leaf});
        witness.append(libzcash::PedersenHash{leaf});
        assert(witness.position() == 0);
        assert(witness.root() == tree.root());
        Check("tree", tree);
        Check("witness", witness);
    }
}
