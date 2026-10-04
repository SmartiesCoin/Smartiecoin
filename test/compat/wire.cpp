// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sapling/zip32.h>
#include <serialize.h>
#include <streams.h>

#include <cassert>
#include <iomanip>
#include <iostream>
#include <optional>
#include <type_traits>
#include <variant>

using namespace libzcash;

template <typename T> auto Bytes(const T& value)
{
    CDataStream stream(SER_NETWORK, 0);
    stream << value;
    return std::vector<std::byte>(stream.begin(), stream.end());
}

struct Pair {
    uint32_t a{0}, b{0};
    SERIALIZE_METHODS(Pair, obj) { READWRITE(obj.a, obj.b); }
};

void Hex(const auto& bytes)
{
    for (auto byte : bytes) std::cout << std::hex << std::setw(2) << std::setfill('0') << int(byte);
    std::cout << std::dec << '\n';
}

template <typename T> void RoundTripAndTruncate(const T& value)
{
    const auto encoded = Bytes(value);
    CDataStream stream(encoded, SER_NETWORK, 0);
    T decoded{};
    stream >> decoded;
    assert(stream.empty() && Bytes(decoded) == encoded);
    for (size_t length = 0; length < encoded.size(); ++length) {
        CDataStream truncated(Span<const std::byte>(encoded.data(), length), SER_NETWORK, 0);
        bool threw{false};
        try { truncated >> decoded; } catch (const std::ios_base::failure&) { threw = true; }
        assert(threw);
    }
}

int main()
{
    Optional<uint32_t> value;
    assert(Bytes(value) == std::vector<std::byte>{std::byte{0}});
    Hex(Bytes(value));
    value = 0x12345678;
    const auto gold = Bytes(value);
    assert((gold == std::vector<std::byte>{std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}}));
    Hex(gold);

    for (int tag = 0; tag < 256; ++tag) {
        auto encoded = gold;
        encoded[0] = std::byte(tag);
        CDataStream stream(encoded, SER_NETWORK, 0);
        Optional<uint32_t> destination = 99;
        stream >> destination;
        assert(tag ? destination && *destination == 0x12345678 : !destination);
        assert(stream.size() == (tag ? 0 : 4));
        // Every incomplete present encoding preserves BOTH possible destination
        // states. Zero is absent and intentionally ignores the remaining bytes.
        for (size_t length = 0; length < (tag ? encoded.size() : 1); ++length) {
            for (bool present : {false, true}) {
                CDataStream truncated(Span<const std::byte>(encoded.data(), length), SER_NETWORK, 0);
                Optional<uint32_t> previous;
                if (present) previous = 99;
                bool threw{false};
                try { truncated >> previous; } catch (const std::ios_base::failure&) { threw = true; }
                assert(threw);
                assert(present ? previous && *previous == 99 : !previous);
            }
        }
    }

    const auto pair_bytes = Bytes(Optional<Pair>{Pair{1, 2}});
    for (size_t length = 0; length < pair_bytes.size(); ++length) {
        CDataStream stream(Span<const std::byte>(pair_bytes.data(), length), SER_NETWORK, 0);
        Optional<Pair> previous{Pair{99, 100}};
        bool threw{false};
        try { stream >> previous; } catch (const std::ios_base::failure&) { threw = true; }
        assert(threw && previous->a == 99 && previous->b == 100);
    }

    Optional<Optional<uint32_t>> nested{Optional<uint32_t>{}};
    assert((Bytes(nested) == std::vector<std::byte>{std::byte{1}, std::byte{0}}));
    Hex(Bytes(nested));
    RoundTripAndTruncate(nested);
    const std::vector<Optional<uint32_t>> vector{nullopt, 0x12345678};
    const auto vector_bytes = Bytes(vector);
    assert((vector_bytes == std::vector<std::byte>{std::byte{2}, std::byte{0}, std::byte{1}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}}));
    Hex(vector_bytes);
    RoundTripAndTruncate(vector);

    SaplingPaymentAddress address;
    for (size_t i = 0; i < address.d.size(); ++i) address.d[i] = i + 1;
    for (size_t i = 0; i < 32; ++i) address.pk_d.begin()[i] = i + 12;
    const auto address_bytes = Bytes(address);
    assert(address_bytes.size() == 43);
    for (size_t i = 0; i < 43; ++i) assert(address_bytes[i] == std::byte(i + 1));
    Hex(address_bytes);
    RoundTripAndTruncate(address);

    SaplingExtendedFullViewingKey viewing{};
    viewing.depth = 5;
    viewing.parentFVKTag = 0x12345678;
    viewing.childIndex = 0xabcdef01;
    // Distinct bytes across all five 32-byte fields detect field reordering.
    const std::array<uint256*, 5> fields{&viewing.chaincode, &viewing.fvk.ak, &viewing.fvk.nk, &viewing.fvk.ovk, &viewing.dk};
    for (size_t field = 0; field < fields.size(); ++field) {
        for (size_t i = 0; i < 32; ++i) fields[field]->begin()[i] = field * 32 + i + 1;
    }
    const auto key_bytes = Bytes(viewing);
    std::vector<std::byte> expected_key(169);
    const std::array<unsigned char, 9> header{5, 0x78, 0x56, 0x34, 0x12, 1, 0xef, 0xcd, 0xab};
    for (size_t i = 0; i < header.size(); ++i) expected_key[i] = std::byte(header[i]);
    for (size_t i = 0; i < 160; ++i) expected_key[9 + i] = std::byte(i + 1);
    assert(key_bytes == expected_key);
    SaplingExtendedSpendingKey spending{};
    spending.depth = viewing.depth;
    spending.parentFVKTag = viewing.parentFVKTag;
    spending.childIndex = viewing.childIndex;
    spending.chaincode = viewing.chaincode;
    spending.expsk.ask = viewing.fvk.ak;
    spending.expsk.nsk = viewing.fvk.nk;
    spending.expsk.ovk = viewing.fvk.ovk;
    spending.dk = viewing.dk;
    assert(Bytes(spending) == key_bytes);
    RoundTripAndTruncate(viewing);
    RoundTripAndTruncate(spending);

    PaymentAddress invalid;
    ViewingKey invalid_viewing;
    SpendingKey invalid_spending;
    assert(invalid.index() == 0 && invalid_viewing.index() == 0 && invalid_spending.index() == 0);
    assert(std::get_if<SaplingPaymentAddress>(&invalid) == nullptr);
    assert(invalid == PaymentAddress{});
    // Compatibility requirement: do not repair this pre-existing anomaly here.
    assert(invalid < PaymentAddress{});
    PaymentAddress valid{address};
    assert(invalid < valid && !(valid < invalid));
    bool bad_access{false};
    try { (void)std::get<SaplingPaymentAddress>(invalid); } catch (const std::bad_variant_access&) { bad_access = true; }
    assert(bad_access);

    std::cout << "traits payment/view/spend nothrow copy+move: "
              << std::is_nothrow_copy_constructible_v<SaplingPaymentAddress>
              << std::is_nothrow_move_constructible_v<SaplingPaymentAddress>
              << std::is_nothrow_copy_constructible_v<SaplingExtendedFullViewingKey>
              << std::is_nothrow_move_constructible_v<SaplingExtendedFullViewingKey>
              << std::is_nothrow_copy_constructible_v<SaplingExtendedSpendingKey>
              << std::is_nothrow_move_constructible_v<SaplingExtendedSpendingKey> << '\n';
    // Stable transcript shared with the pre-migration Boost probe.
    std::cout << "PASS all 256 tags; all truncation offsets retained destinations; nested/vector order; address 43/key 169 bytes; invalid-first default, pointer miss, bad_get, anomalous invalid<invalid\n";
}
