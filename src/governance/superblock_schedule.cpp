// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/superblock_schedule.h>

#include <chainparams.h>

bool superblock_schedule::IsValidBlockHeight(int nBlockHeight)
{
    const Consensus::Params& consensusParams = Params().GetConsensus();
    if (nBlockHeight < consensusParams.nSuperblockStartBlock) {
        return false;
    }

    if (nBlockHeight < consensusParams.nSMTv040Height) {
        return nBlockHeight % consensusParams.nSuperblockCycle == 0;
    }

    if (nBlockHeight == consensusParams.nSMTv040Height) {
        // The fork height was chosen as an existing superblock height so the
        // pre-fork governance cycle can close without losing approved payouts.
        return nBlockHeight % consensusParams.nSuperblockCycle == 0;
    }

    return (nBlockHeight - consensusParams.nSMTv040Height) % consensusParams.nSMTv040SuperblockCycle == 0;
}

int superblock_schedule::GetPaymentCycle(int nBlockHeight)
{
    return Params().GetConsensus().SuperblockCycle(nBlockHeight);
}
