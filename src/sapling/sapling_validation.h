// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020-2021 The PIVX Core developers
// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef SMARTIECOIN_SAPLING_SAPLING_VALIDATION_H
#define SMARTIECOIN_SAPLING_SAPLING_VALIDATION_H

#include <consensus/amount.h>

class CBlockIndex;
class CTransaction;
class TxValidationState;

namespace Consensus {
struct Params;
}

namespace SaplingValidation {

bool CheckTransaction(const CTransaction& tx, TxValidationState& state, CAmount& nValueOut);
bool CheckTransactionWithoutProofVerification(const CTransaction& tx, TxValidationState& state, CAmount& nValueOut);
bool ContextualCheckTransaction(const CTransaction& tx, TxValidationState& state, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev);

} // namespace SaplingValidation

#endif // SMARTIECOIN_SAPLING_SAPLING_VALIDATION_H
