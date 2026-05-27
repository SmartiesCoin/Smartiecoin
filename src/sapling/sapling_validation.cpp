// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020-2021 The PIVX Core developers
// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <sapling/sapling_validation.h>

#include <chain.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>

#include <librustzcash.h>

#include <limits>
#include <set>

namespace SaplingValidation {

bool CheckTransaction(const CTransaction& tx, TxValidationState& state, CAmount& nValueOut)
{
    if (!tx.HasShieldedPayload()) {
        return true;
    }

    if (!tx.IsShieldedTxVersion()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-form-not-sapling");
    }

    if (tx.IsCoinBase()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cb-has-shielded-data");
    }

    return CheckTransactionWithoutProofVerification(tx, state, nValueOut);
}

bool CheckTransactionWithoutProofVerification(const CTransaction& tx, TxValidationState& state, CAmount& nValueOut)
{
    const SaplingTxData& sapData = tx.sapData;

    if (sapData.vShieldedSpend.empty() && sapData.vShieldedOutput.empty() && sapData.valueBalance != 0) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-valuebalance-nonzero");
    }

    if (sapData.valueBalance > MAX_MONEY || sapData.valueBalance < -MAX_MONEY) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-valuebalance-toolarge");
    }

    if (sapData.valueBalance < 0) {
        nValueOut += -sapData.valueBalance;
        if (!MoneyRange(nValueOut)) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-txouttotal-toolarge");
        }
    }

    if (sapData.valueBalance > 0 && !MoneyRange(sapData.valueBalance)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-txintotal-toolarge");
    }

    std::set<uint256> nullifiers;
    for (const SpendDescription& spend_desc : sapData.vShieldedSpend) {
        if (!nullifiers.insert(spend_desc.nullifier).second) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-spend-description-nullifiers-duplicate");
        }
    }

    return true;
}

bool ContextualCheckTransaction(const CTransaction& tx, TxValidationState& state, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    const bool shieldActive = DeploymentActiveAfter(pindexPrev, consensusParams, Consensus::DEPLOYMENT_SMT_SHIELD);

    if (tx.IsShieldedTxVersion() && !shieldActive) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-shield-not-active");
    }

    if (!tx.HasShieldedPayload()) {
        return true;
    }

    if (tx.IsCoinBase()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cb-has-shielded-data");
    }

    uint256 dataToBeSigned;
    try {
        CScript scriptCode;
        dataToBeSigned = SignatureHash(scriptCode, tx, NOT_AN_INPUT, SIGHASH_ALL, 0, SigVersion::SAPLING);
    } catch (const std::exception&) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "error-computing-sapling-signature-hash");
    }

    auto ctx = librustzcash_sapling_verification_ctx_init();

    for (const SpendDescription& spend : tx.sapData.vShieldedSpend) {
        if (!librustzcash_sapling_check_spend(
                ctx,
                spend.cv.begin(),
                spend.anchor.begin(),
                spend.nullifier.begin(),
                spend.rk.begin(),
                spend.zkproof.begin(),
                spend.spendAuthSig.begin(),
                dataToBeSigned.begin())) {
            librustzcash_sapling_verification_ctx_free(ctx);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-sapling-spend-description-invalid");
        }
    }

    for (const OutputDescription& output : tx.sapData.vShieldedOutput) {
        if (!librustzcash_sapling_check_output(
                ctx,
                output.cv.begin(),
                output.cmu.begin(),
                output.ephemeralKey.begin(),
                output.zkproof.begin())) {
            librustzcash_sapling_verification_ctx_free(ctx);
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-sapling-output-description-invalid");
        }
    }

    if (!librustzcash_sapling_final_check(
            ctx,
            tx.sapData.valueBalance,
            tx.sapData.bindingSig.begin(),
            dataToBeSigned.begin())) {
        librustzcash_sapling_verification_ctx_free(ctx);
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-sapling-binding-signature-invalid");
    }

    librustzcash_sapling_verification_ctx_free(ctx);
    return true;
}

} // namespace SaplingValidation
