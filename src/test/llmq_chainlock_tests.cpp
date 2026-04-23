// Copyright (c) 2025 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/llmq_tests.h>
#include <test/util/setup_common.h>

#include <streams.h>
#include <util/strencodings.h>

#include <chainlock/clsig.h>
#include <chainlock/handler.h>

#include <boost/test/unit_test.hpp>

using chainlock::ChainLockSig;
using namespace llmq;
using namespace llmq::testutils;
using namespace std::chrono_literals;

BOOST_FIXTURE_TEST_SUITE(llmq_chainlock_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(wait_for_islock_timeout_is_60s)
{
    // Regression guard: Smartiecoin uses 60 s (one 60-s block of propagation
    // slack). Dash upstream defaults to 10 min, which on Smartiecoin's small
    // MN set caused every tx to sit in the mempool for ~10 blocks because IS
    // locks rarely form. Bumping this back to 10 min would reintroduce that
    // bug, so the value is pinned by this test.
    BOOST_CHECK_EQUAL(chainlock::WAIT_FOR_ISLOCK_TIMEOUT.count(), 60);
}

BOOST_AUTO_TEST_CASE(islock_safe_for_mining_threshold_behavior)
{
    // IsTxSafeForMining uses `tx_age >= WAIT_FOR_ISLOCK_TIMEOUT`. Exercise the
    // threshold at a few ages to confirm the boundary is inclusive at 60 s.
    const auto safe = [](std::chrono::seconds tx_age) {
        return tx_age >= chainlock::WAIT_FOR_ISLOCK_TIMEOUT;
    };
    BOOST_CHECK(!safe(0s));     // tx just relayed
    BOOST_CHECK(!safe(30s));    // mid-window
    BOOST_CHECK(!safe(59s));    // one second short
    BOOST_CHECK(safe(60s));     // exactly at threshold — now minable
    BOOST_CHECK(safe(61s));     // past threshold
    BOOST_CHECK(safe(10min));   // at the old 10-min default, tx is abundantly safe
}

BOOST_AUTO_TEST_CASE(chainlock_construction_test)
{
    // Test default constructor
    ChainLockSig clsig1;
    BOOST_CHECK(clsig1.IsNull());
    BOOST_CHECK_EQUAL(clsig1.getHeight(), -1);
    BOOST_CHECK(clsig1.getBlockHash().IsNull());
    BOOST_CHECK(!clsig1.getSig().IsValid());

    // Test parameterized constructor
    int32_t height = 12345;
    uint256 blockHash = GetTestBlockHash(1);
    CBLSSignature sig = CreateRandomBLSSignature();

    ChainLockSig clsig2(height, blockHash, sig);
    BOOST_CHECK(!clsig2.IsNull());
    BOOST_CHECK_EQUAL(clsig2.getHeight(), height);
    BOOST_CHECK(clsig2.getBlockHash() == blockHash);
    BOOST_CHECK(clsig2.getSig() == sig);
}

BOOST_AUTO_TEST_CASE(chainlock_null_test)
{
    ChainLockSig clsig;

    // Default constructed should be null
    BOOST_CHECK(clsig.IsNull());

    // With height set but null hash, should not be null
    clsig = ChainLockSig(100, uint256(), CBLSSignature());
    BOOST_CHECK(!clsig.IsNull());

    // With valid data should not be null
    clsig = CreateChainLock(100, GetTestBlockHash(1));
    BOOST_CHECK(!clsig.IsNull());
}

BOOST_AUTO_TEST_CASE(chainlock_serialization_test)
{
    // Test with valid chainlock
    ChainLockSig clsig = CreateChainLock(67890, GetTestBlockHash(42));

    // Test serialization preserves all fields
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << clsig;

    ChainLockSig deserialized;
    ss >> deserialized;

    BOOST_CHECK_EQUAL(clsig.getHeight(), deserialized.getHeight());
    BOOST_CHECK(clsig.getBlockHash() == deserialized.getBlockHash());
    BOOST_CHECK(clsig.getSig() == deserialized.getSig());
    BOOST_CHECK_EQUAL(clsig.IsNull(), deserialized.IsNull());
}

BOOST_AUTO_TEST_CASE(chainlock_tostring_test)
{
    // Test null chainlock
    ChainLockSig nullClsig;
    std::string nullStr = nullClsig.ToString();
    BOOST_CHECK(!nullStr.empty());

    // Test valid chainlock
    int32_t height = 123456;
    uint256 blockHash = GetTestBlockHash(789);
    ChainLockSig clsig = CreateChainLock(height, blockHash);

    std::string str = clsig.ToString();
    BOOST_CHECK(!str.empty());

    // ToString should contain height and hash info
    BOOST_CHECK(str.find(strprintf("%d", height)) != std::string::npos);
    BOOST_CHECK(str.find(blockHash.ToString().substr(0, 10)) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(chainlock_edge_cases_test)
{
    // Test with edge case heights
    ChainLockSig clsig1 = CreateChainLock(0, GetTestBlockHash(1));
    BOOST_CHECK_EQUAL(clsig1.getHeight(), 0);
    BOOST_CHECK(!clsig1.IsNull());

    ChainLockSig clsig2 = CreateChainLock(std::numeric_limits<int32_t>::max(), GetTestBlockHash(2));
    BOOST_CHECK_EQUAL(clsig2.getHeight(), std::numeric_limits<int32_t>::max());

    // Test serialization with extreme values
    CDataStream ss1(SER_NETWORK, PROTOCOL_VERSION);
    ss1 << clsig1;
    ChainLockSig clsig1_deserialized;
    ss1 >> clsig1_deserialized;
    BOOST_CHECK_EQUAL(clsig1.getHeight(), clsig1_deserialized.getHeight());

    CDataStream ss2(SER_NETWORK, PROTOCOL_VERSION);
    ss2 << clsig2;
    ChainLockSig clsig2_deserialized;
    ss2 >> clsig2_deserialized;
    BOOST_CHECK_EQUAL(clsig2.getHeight(), clsig2_deserialized.getHeight());
}

BOOST_AUTO_TEST_CASE(chainlock_comparison_test)
{
    // Create identical chainlocks
    int32_t height = 5000;
    uint256 blockHash = GetTestBlockHash(10);
    CBLSSignature sig = CreateRandomBLSSignature();

    ChainLockSig clsig1(height, blockHash, sig);
    ChainLockSig clsig2(height, blockHash, sig);

    // Verify getters return same values
    BOOST_CHECK_EQUAL(clsig1.getHeight(), clsig2.getHeight());
    BOOST_CHECK(clsig1.getBlockHash() == clsig2.getBlockHash());
    BOOST_CHECK(clsig1.getSig() == clsig2.getSig());

    // Different chainlocks
    ChainLockSig clsig3(height + 1, blockHash, sig);
    BOOST_CHECK(clsig1.getHeight() != clsig3.getHeight());

    ChainLockSig clsig4(height, GetTestBlockHash(11), sig);
    BOOST_CHECK(clsig1.getBlockHash() != clsig4.getBlockHash());
}

BOOST_AUTO_TEST_CASE(chainlock_malformed_data_test)
{
    // Test deserialization of truncated data
    ChainLockSig clsig = CreateChainLock(1000, GetTestBlockHash(5));

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << clsig;

    // Truncate the stream
    std::string data = ss.str();
    for (size_t truncateAt = 1; truncateAt < data.size(); truncateAt += 10) {
        CDataStream truncated(std::vector<unsigned char>(data.begin(), data.begin() + truncateAt), SER_NETWORK,
                              PROTOCOL_VERSION);

        ChainLockSig deserialized;
        try {
            truncated >> deserialized;
            // If no exception, verify it's either complete or default
            if (truncateAt < sizeof(int32_t)) {
                BOOST_CHECK(deserialized.IsNull());
            }
        } catch (const std::exception&) {
            // Expected for most truncation points
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
