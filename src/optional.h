// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPTIONAL_H
#define BITCOIN_OPTIONAL_H

#include <optional>

template <typename T>
using Optional = std::optional<T>;

inline constexpr auto nullopt = std::nullopt;

#endif // BITCOIN_OPTIONAL_H
