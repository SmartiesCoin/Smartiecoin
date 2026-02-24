// Copyright (c) 2014-2025 The Smartiecoin Core developers
// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HASH_X11_H
#define BITCOIN_HASH_X11_H

#include <crypto/yespower/yespower.h>

#include <uint256.h>

#include <algorithm>
#include <cstring>

extern "C" int yespower_hash(const char* input, char* output);

/* ----------- Smartiecoin PoW (YesPower) ------------------------------- */
template <typename T1>
inline uint256 HashX11(const T1 pbegin, const T1 pend)
{
    unsigned char header[80]{};
    const std::size_t input_size = static_cast<std::size_t>(pend - pbegin) * sizeof(pbegin[0]);
    const std::size_t copy_size = std::min<std::size_t>(sizeof(header), input_size);
    if (copy_size != 0) {
        std::memcpy(header, static_cast<const void*>(&pbegin[0]), copy_size);
    }

    uint256 result;
    yespower_hash(reinterpret_cast<const char*>(header), reinterpret_cast<char*>(&result));
    return result;
}

#endif // BITCOIN_HASH_X11_H
