// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <governance/classes.h>
#include <governance/superblock_schedule.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <set>

BOOST_FIXTURE_TEST_SUITE(superblock_schedule_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(network_boundaries)
{
    // No node or networking: SelectParams only selects the consensus parameters.
    gArgs.ForceSetArg("-devnet", "schedule-test");
    for (const auto* network : {"main", "test", "devnet", "regtest"}) {
        SelectParams(network);
        const auto& params = Params().GetConsensus();
        const int start = params.nSuperblockStartBlock;
        const int fork = params.nSMTv040Height;
        const int old_cycle = params.nSuperblockCycle;
        const int new_cycle = params.nSMTv040SuperblockCycle;
        const int first = start + (old_cycle - start % old_cycle) % old_cycle;
        const std::set<int> heights{
            std::numeric_limits<int>::min(), -1, 0, 1,
            start - 1, start, start + 1, first - 1, first, first + 1,
            fork - old_cycle, fork - 1, fork, fork + 1,
            fork + new_cycle - 1, fork + new_cycle, fork + new_cycle + 1,
            std::numeric_limits<int>::max() - 1, std::numeric_limits<int>::max()};
        for (int height : heights) {
            BOOST_CHECK_EQUAL(superblock_schedule::GetPaymentCycle(height), CSuperblock::GetPaymentCycle(height));
            BOOST_CHECK_EQUAL(superblock_schedule::IsValidBlockHeight(height), CSuperblock::IsValidBlockHeight(height));
            BOOST_CHECK_EQUAL(superblock_schedule::GetPaymentCycle(height), params.SuperblockCycle(height));
        }
        BOOST_CHECK(!superblock_schedule::IsValidBlockHeight(start - 1));
        BOOST_CHECK(superblock_schedule::IsValidBlockHeight(first));
        // Exact fork uses the OLD alignment, not the post-fork offset formula.
        BOOST_CHECK_EQUAL(superblock_schedule::IsValidBlockHeight(fork), fork % old_cycle == 0);
        BOOST_CHECK(!superblock_schedule::IsValidBlockHeight(fork + 1));
        BOOST_CHECK(superblock_schedule::IsValidBlockHeight(fork + new_cycle));
    }
    SelectParams("regtest");
}

BOOST_AUTO_TEST_SUITE_END()
