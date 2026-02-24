// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(subsidy_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(block_subsidy_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

    static constexpr CAmount kInitialSubsidy = 50 * COIN;
    static constexpr int kHalvingInterval = 1030596;

    uint32_t nPrevBits;
    int32_t nPrevHeight;
    CAmount nSubsidy;

    BOOST_CHECK_EQUAL(chainParams->GetConsensus().nSubsidyHalvingInterval, kHalvingInterval);

    // Mainnet starts at 50 SMT subsidy.
    nPrevBits = 0x1e3fffff;
    nPrevHeight = 1;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // V20 does not affect subsidy scheduling on Smartiecoin mainnet/testnet.
    nPrevBits = 0x1b10d50b;
    nPrevHeight = 4249;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(nSubsidy, kInitialSubsidy);

    // Halving boundaries (nPrevHeight semantics: subsidy returned for block nPrevHeight + 1).
    nPrevBits = 0x1b10d50b;
    nPrevHeight = kHalvingInterval - 1;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 50 * COIN);

    nPrevHeight = kHalvingInterval;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 25 * COIN);

    nPrevHeight = 2 * kHalvingInterval;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 1250000000); // 12.5 SMT

    // With H=1,030,596, cap is reached after block 5,256,028.
    nPrevHeight = 5256027;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 156250000); // 1.5625 SMT

    nPrevHeight = 5256028;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 0);
}

BOOST_AUTO_TEST_SUITE_END()
