// Copyright (c) 2014-2023 The Smartiecoin Core developers
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
    static constexpr int kOldHalvingInterval = 1030596;
    static constexpr int kNewHalvingInterval = 1000000;

    uint32_t nPrevBits;
    int32_t nPrevHeight;
    CAmount nSubsidy;

    // chainparams keeps the old interval; the new one activates at nSMTv014Height
    BOOST_CHECK_EQUAL(chainParams->GetConsensus().nSubsidyHalvingInterval, kOldHalvingInterval);

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

    // Halving boundaries after v0.1.4 fork uses new interval (1,000,000).
    // nPrevHeight semantics: subsidy returned for block nPrevHeight + 1.
    nPrevBits = 0x1b10d50b;
    nPrevHeight = kNewHalvingInterval - 1;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 50 * COIN);

    nPrevHeight = kNewHalvingInterval;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 25 * COIN);

    nPrevHeight = 2 * kNewHalvingInterval;
    nSubsidy = GetBlockSubsidyInner(nPrevBits, nPrevHeight, chainParams->GetConsensus(), /*fV20Active=*/ false);
    BOOST_CHECK_EQUAL(nSubsidy, 1250000000); // 12.5 SMT
}

BOOST_AUTO_TEST_CASE(masternode_payment_v014_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int nSMTv014Height = chainParams->GetConsensus().nSMTv014Height;
    const int nSMTv030Height = chainParams->GetConsensus().nSMTv030Height;

    // After v0.1.4 fork: MN gets 50% of distributable (= 45% of total subsidy)
    // blockValue passed to GetMasternodePayment is already after treasury deduction
    CAmount blockValue = 45 * COIN; // 50 SMT - 10% treasury = 45 SMT
    CAmount mnPayment = GetMasternodePayment(nSMTv014Height, blockValue, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, blockValue / 2); // 22.5 SMT

    // Miner gets the other half
    CAmount minerPayment = blockValue - mnPayment;
    BOOST_CHECK_EQUAL(minerPayment, blockValue / 2); // 22.5 SMT

    // One block before v0.3.0 fork still uses v0.1.4 50/50 split
    CAmount blockValueHalved = 2250000000; // 22.5 SMT (pretend subsidy halved)
    mnPayment = GetMasternodePayment(nSMTv030Height - 1, blockValueHalved, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, blockValueHalved / 2); // 11.25 SMT
}

BOOST_AUTO_TEST_CASE(masternode_payment_v030_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const int nSMTv030Height = chainParams->GetConsensus().nSMTv030Height;

    // After v0.3.0 fork: 18/72/10 split.
    // Treasury (10%) is already deducted from blockValue, so blockValue = 90% of subsidy.
    // MN = blockValue * 4/5 = 80% of blockValue = 72% of subsidy.
    // Miner = blockValue * 1/5 = 20% of blockValue = 18% of subsidy.
    CAmount blockValue = 45 * COIN; // 50 SMT - 10% treasury = 45 SMT
    CAmount mnPayment = GetMasternodePayment(nSMTv030Height, blockValue, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, 36 * COIN); // 72% of 50 SMT
    BOOST_CHECK_EQUAL(mnPayment, blockValue * 4 / 5);

    CAmount minerPayment = blockValue - mnPayment;
    BOOST_CHECK_EQUAL(minerPayment, 9 * COIN); // 18% of 50 SMT

    // Same ratios apply at any height >= nSMTv030Height (subsidy halving is independent).
    CAmount blockValueHalved = 2250000000; // 22.5 SMT post-halving distributable
    mnPayment = GetMasternodePayment(nSMTv030Height + 1000000, blockValueHalved, /*fV20Active=*/ true);
    BOOST_CHECK_EQUAL(mnPayment, 1800000000); // 18 SMT = 72% of 25 SMT base
}

BOOST_AUTO_TEST_SUITE_END()
