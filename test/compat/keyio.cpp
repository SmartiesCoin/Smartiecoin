// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bech32.h>
#include <sapling/key_io_sapling.h>

#include <cassert>
#include <iostream>
#include <variant>

// Isolated HRP fixture: no daemon, network, wallet, or key derivation.
struct ProbeParams : CChainParams {
    ProbeParams()
    {
        bech32HRPs[SAPLING_PAYMENT_ADDRESS] = "zs";
        bech32HRPs[SAPLING_EXTENDED_FVK] = "zxviews";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY] = "secret-extended-key-main";
    }
};

const CChainParams& Params()
{
    static ProbeParams params;
    return params;
}

void CheckMalformed(const std::string& encoded, const auto& decode)
{
    for (const auto& bad : {std::string{}, std::string{"invalid"}, encoded.substr(1), encoded + "q"}) {
        assert(decode(bad).index() == 0);
    }
    auto parsed = bech32::Decode(encoded, /*limit_length=*/false);
    assert(parsed.encoding == bech32::Encoding::BECH32);
    for (auto encoding : {bech32::Encoding::BECH32, bech32::Encoding::BECH32M}) {
        assert(decode(bech32::Encode(encoding, "wrong", parsed.data)).index() == 0);
    }
    assert(decode(bech32::Encode(bech32::Encoding::BECH32M, parsed.hrp, parsed.data)).index() == 0);
    // Both 43-byte addresses and 169-byte keys leave unused low padding bits.
    parsed.data.back() |= 1;
    assert(decode(bech32::Encode(bech32::Encoding::BECH32, parsed.hrp, parsed.data)).index() == 0);
}

int main()
{
    using namespace libzcash;
    assert(KeyIO::EncodePaymentAddress(PaymentAddress{}).empty());
    assert(KeyIO::EncodeViewingKey(ViewingKey{}).empty());
    assert(KeyIO::EncodeSpendingKey(SpendingKey{}).empty());
    assert(!IsValidPaymentAddress(PaymentAddress{}));
    assert(!IsValidViewingKey(ViewingKey{}));
    assert(!IsValidSpendingKey(SpendingKey{}));

    SaplingPaymentAddress address;
    for (size_t i = 0; i < 11; ++i) address.d[i] = i + 1;
    for (size_t i = 0; i < 32; ++i) address.pk_d.begin()[i] = i + 12;
    const auto encoded_address = KeyIO::EncodePaymentAddress(address);
    const auto decoded_address = KeyIO::DecodePaymentAddress(encoded_address);
    assert(IsValidPaymentAddress(decoded_address));
    assert(*std::get_if<SaplingPaymentAddress>(&decoded_address) == address);
    const auto optional_address = KeyIO::DecodeSaplingPaymentAddress(encoded_address);
    assert(optional_address && *optional_address == address);
    assert(KeyIO::IsValidPaymentAddressString(encoded_address));
    std::cout << encoded_address << '\n';
    CheckMalformed(encoded_address, KeyIO::DecodePaymentAddress);
    for (const auto& bad : {std::string{}, std::string{"invalid"}, encoded_address.substr(1), encoded_address + "q"}) {
        assert(!KeyIO::DecodeSaplingPaymentAddress(bad));
        assert(!KeyIO::IsValidPaymentAddressString(bad));
    }

    SaplingExtendedFullViewingKey viewing{};
    viewing.depth = 7;
    viewing.parentFVKTag = 0x12345678;
    viewing.childIndex = 0xabcdef01;
    const auto encoded_viewing = KeyIO::EncodeViewingKey(viewing);
    const auto decoded_viewing = KeyIO::DecodeViewingKey(encoded_viewing);
    assert(IsValidViewingKey(decoded_viewing));
    assert(*std::get_if<SaplingExtendedFullViewingKey>(&decoded_viewing) == viewing);
    assert(KeyIO::DecodeSpendingKey(encoded_viewing).index() == 0);
    CheckMalformed(encoded_viewing, KeyIO::DecodeViewingKey);
    std::cout << encoded_viewing << '\n';

    SaplingExtendedSpendingKey spending{};
    spending.depth = 7;
    spending.parentFVKTag = 0x12345678;
    spending.childIndex = 0xabcdef01;
    const auto encoded_spending = KeyIO::EncodeSpendingKey(spending);
    const auto decoded_spending = KeyIO::DecodeSpendingKey(encoded_spending);
    assert(IsValidSpendingKey(decoded_spending));
    assert(*std::get_if<SaplingExtendedSpendingKey>(&decoded_spending) == spending);
    const auto invalid_key = KeyIO::DecodeSpendingKey(encoded_viewing);
    assert(std::get_if<SaplingExtendedSpendingKey>(&invalid_key) == nullptr);
    assert(KeyIO::DecodeViewingKey(encoded_spending).index() == 0);
    CheckMalformed(encoded_spending, KeyIO::DecodeSpendingKey);
    std::cout << encoded_spending << '\n';
    std::cout << "PASS actual KeyIO encoder/decoder visitors: nil, payment optional, view/spend roundtrip, wrong HRP/type/checksum/padding, BECH32M rejection; synthetic public byte patterns only\n";
}
