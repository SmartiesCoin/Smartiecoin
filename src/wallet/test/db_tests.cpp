// Copyright (c) 2018-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <fs.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <wallet/bdb.h>

#include <fstream>
#include <memory>
#include <string>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(db_tests, BasicTestingSetup)

static std::shared_ptr<BerkeleyEnvironment> GetWalletEnv(const fs::path& path, fs::path& database_filename)
{
    fs::path data_file = BDBDataFile(path);
    database_filename = data_file.filename();
    return GetBerkeleyEnv(data_file.parent_path(), false);
}

BOOST_AUTO_TEST_CASE(getwalletenv_file)
{
    fs::path test_name = "test_name.dat";
    const fs::path datadir = m_args.GetDataDirNet();
    fs::path file_path = datadir / test_name;
    std::ofstream f{file_path};
    f.close();

    fs::path filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(file_path, filename);
    BOOST_CHECK_EQUAL(filename, test_name);
    BOOST_CHECK_EQUAL(env->Directory(), datadir);
}

BOOST_AUTO_TEST_CASE(getwalletenv_directory)
{
    fs::path expected_name = "wallet.dat";
    const fs::path datadir = m_args.GetDataDirNet();

    fs::path filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(datadir, filename);
    BOOST_CHECK_EQUAL(filename, expected_name);
    BOOST_CHECK_EQUAL(env->Directory(), datadir);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_multiple)
{
    fs::path datadir = m_args.GetDataDirNet() / "1";
    fs::path datadir_2 = m_args.GetDataDirNet() / "2";
    fs::path filename;

    std::shared_ptr<BerkeleyEnvironment> env_1 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_3 = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1 == env_2);
    BOOST_CHECK(env_2 != env_3);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_free_instance)
{
    fs::path datadir = gArgs.GetDataDirNet() / "1";
    fs::path datadir_2 = gArgs.GetDataDirNet() / "2";
    fs::path filename;

    std::shared_ptr <BerkeleyEnvironment> env_1_a = GetWalletEnv(datadir, filename);
    std::shared_ptr <BerkeleyEnvironment> env_2_a = GetWalletEnv(datadir_2, filename);
    env_1_a.reset();

    std::shared_ptr<BerkeleyEnvironment> env_1_b = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2_b = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1_a != env_1_b);
    BOOST_CHECK(env_2_a == env_2_b);
}

// Recovery must not discard transaction logs based on historical diagnostic text.
BOOST_AUTO_TEST_CASE(recovery_failure_preserves_environment)
{
    for (const std::string& diagnostic : {std::string{}, std::string{"old unrelated version message\n"}}) {
        const fs::path dir = m_args.GetDataDirNet() / (diagnostic.empty() ? "no-message" : "stale-message");
        fs::create_directories(dir / "database");
        // A disposable synthetic wallet and corrupt transaction log, never user data.
        const std::string wallet_bytes = "synthetic wallet must remain unchanged";
        const std::string log_bytes(4096, 'x');
        std::ofstream{dir / "wallet.dat", std::ios::binary} << wallet_bytes;
        std::ofstream{dir / "database" / "log.0000000001", std::ios::binary} << log_bytes;
        std::ofstream{dir / "db.log"} << diagnostic;
        auto env = GetBerkeleyEnv(dir, false);
        bilingual_str error;
        BOOST_CHECK(!env->Open(error));
        BOOST_CHECK(!env->IsInitialized());
        BOOST_CHECK(error.original.find("Berkeley DB recovery failed") != std::string::npos);
        BOOST_CHECK(error.original.find("Back up the entire wallet directory") != std::string::npos);
        const auto contents = [](const fs::path& path) {
            std::ifstream file{path, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        };
        BOOST_CHECK_EQUAL(contents(dir / "wallet.dat"), wallet_bytes);
        BOOST_CHECK_EQUAL(contents(dir / "database" / "log.0000000001"), log_bytes);
        for (const auto& entry : fs::directory_iterator(dir)) {
            BOOST_CHECK(fs::PathToString(entry.path().filename()).find("bdb-env-backup-") != 0);
        }
    }
}

// A clean legacy wallet still opens and persists writes despite old errors.
BOOST_AUTO_TEST_CASE(clean_legacy_wallet_reopens_with_stale_diagnostics)
{
    const fs::path dir = m_args.GetDataDirNet() / "clean-legacy";
    fs::create_directories(dir);
    DatabaseOptions options;
    options.use_shared_memory = false;
    DatabaseStatus status;
    bilingual_str error;
    {
        auto db = MakeBerkeleyDatabase(dir, options, status, error);
        BOOST_REQUIRE(db);
        auto batch = db->MakeBatch();
        BOOST_REQUIRE(batch->TxnBegin());
        BOOST_REQUIRE(batch->Write(std::string{"test-key"}, std::string{"test-value"}));
        BOOST_REQUIRE(batch->TxnCommit());
    }
    std::ofstream{dir / "db.log", std::ios::app} << "old unsupported DB version / Version mismatch\n";
    for (int reopen = 0; reopen < 2; ++reopen) {
        auto db = MakeBerkeleyDatabase(dir, options, status, error);
        BOOST_REQUIRE(db);
        auto batch = db->MakeBatch();
        std::string value;
        BOOST_REQUIRE(batch->Read(std::string{"test-key"}, value));
        BOOST_CHECK_EQUAL(value, "test-value");
        BOOST_REQUIRE(batch->Write(std::string{"new-key"}, reopen));
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        BOOST_CHECK(fs::PathToString(entry.path().filename()).find("bdb-env-backup-") != 0);
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
