// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/sapling_service.h>
#include <wallet/sapling_wallet.h>
#include <wallet/wallet.h>
#include <wallet/bdb.h>
#ifdef USE_SQLITE
#include <wallet/sqlite.h>
#include <sqlite3.h>
#endif
#include <wallet/hdchain.h>
#include <sapling/note.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <boost/test/unit_test.hpp>
#include <db_cxx.h>
#include <thread>
#ifndef WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wallet {
// Inspect complete auxiliary state: public note equality excludes witnesses.
struct SaplingWalletTestAccess {
    static auto& Spends(SaplingService& s) { return static_cast<SaplingWallet&>(s).m_sapling_spends; }
    static auto& Nullifiers(SaplingService& s) { return static_cast<SaplingWallet&>(s).m_nullifiers_to_notes; }
};
namespace {
// Pass already-serialized records through the public backend interface unchanged.
struct Raw {
    CDataStream& s;
    template <typename Stream> void Serialize(Stream& out) const { out.write(Span{s.data(), s.size()}); }
    template <typename Stream> void Unserialize(Stream& in) {
        s.resize(in.size()); in.read(Span{s.data(), s.size()});
    }
};
struct Fault {
    std::string write, erase;
    bool begin{false};
    int commit_failure{0}; // 1: abort before returning false; 2: commit then false.
    bool abort_failure{false};
    int commits{0}, aborts{0};
    int fail_nth{0}, matched{0};
};
std::string Tag(const CDataStream& s) { auto copy = s; std::string tag; copy >> tag; return tag; }
class FaultBatch final : public DatabaseBatch {
    std::unique_ptr<DatabaseBatch> inner;
    Fault& f;
    bool ReadKey(CDataStream&& k, CDataStream& v) override { Raw out{v}; return inner->Read(Raw{k}, out); }
    bool WriteKey(CDataStream&& k, CDataStream&& v, bool overwrite) override {
        if (Tag(k) == f.write && (++f.matched == f.fail_nth || f.fail_nth == 0)) return false;
        return inner->Write(Raw{k}, Raw{v}, overwrite);
    }
    bool EraseKey(CDataStream&& k) override { return Tag(k) != f.erase && inner->Erase(Raw{k}); }
    bool HasKey(CDataStream&& k) override { return inner->Exists(Raw{k}); }
public:
    FaultBatch(std::unique_ptr<DatabaseBatch> b, Fault& faults) : inner(std::move(b)), f(faults) {}
    void Flush() override { inner->Flush(); }
    void Close() override { inner->Close(); }
    bool StartCursor() override { return inner->StartCursor(); }
    bool ReadAtCursor(CDataStream& k, CDataStream& v, bool& done) override { return inner->ReadAtCursor(k,v,done); }
    void CloseCursor() override { inner->CloseCursor(); }
    bool TxnBegin() override { return !f.begin && inner->TxnBegin(); }
    bool TxnCommit() override {
        ++f.commits;
        if (f.commit_failure==1) { inner->TxnAbort(); return false; }
        bool ok=inner->TxnCommit(); return ok && f.commit_failure!=2;
    }
    bool TxnAbort() override { ++f.aborts; bool ok=inner->TxnAbort(); return ok && !f.abort_failure; }
};
class FaultDB final : public DummyDatabase {
    std::unique_ptr<WalletDatabase> inner;
public:
    Fault fault;
    explicit FaultDB(std::unique_ptr<WalletDatabase> db) : inner(std::move(db)) {}
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush=true) override { return std::make_unique<FaultBatch>(inner->MakeBatch(flush), fault); }
    bool Rewrite(const char* skip=nullptr) override { return inner->Rewrite(skip); }
    void ReloadDbEnv() override { inner->ReloadDbEnv(); }
    std::string Filename() override { return inner->Filename(); }
    std::string Format() override { return inner->Format(); }
};
std::unique_ptr<WalletDatabase> Open(const fs::path& path) {
    DatabaseOptions options; DatabaseStatus status; bilingual_str error;
    auto db = MakeBerkeleyDatabase(path, options, status, error);
    BOOST_REQUIRE_MESSAGE(db, error.original); return db;
}
void Absent(CWallet& wallet, const libzcash::SaplingExtendedSpendingKey& sk) {
    auto& sapling=wallet.GetSaplingWallet();
    BOOST_CHECK(!sapling.HaveSpendingKeyForPaymentAddress(sk.DefaultAddress()));
    libzcash::SaplingExtendedSpendingKey loaded;
    BOOST_CHECK(!sapling.GetSpendingKeyForPaymentAddress(sk.DefaultAddress(), loaded));
    std::set<libzcash::SaplingPaymentAddress> addresses; sapling.GetPaymentAddresses(addresses);
    BOOST_CHECK_EQUAL(addresses.count(sk.DefaultAddress()), 0U);
}
void NoRecords(WalletDatabase& db, const libzcash::SaplingExtendedSpendingKey& sk) {
    auto b=db.MakeBatch(); auto ivk=sk.ToXFVK().fvk.in_viewing_key();
    BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_KEY,ivk)));
    BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_KEYMETA,ivk)));
    BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_KEY_CRIPTED,sk.ToXFVK())));
    BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_ADDR,sk.DefaultAddress())));
}
}
BOOST_FIXTURE_TEST_SUITE(sapling_storage_tests, BasicTestingSetup)
BOOST_AUTO_TEST_CASE(pinned_backend_version) {
    int major,minor; DbEnv::version(&major,&minor,nullptr);
    BOOST_CHECK_EQUAL(major,4); BOOST_CHECK_EQUAL(minor,8);
}
BOOST_AUTO_TEST_CASE(add_failure_matrix) {
    const std::vector<std::string> faults{"begin",DBKeys::SAP_KEYMETA,DBKeys::SAP_KEY,DBKeys::SAP_ADDR};
    for (size_t i=0;i<faults.size();++i) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("add-%u",i));
        const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        {
            auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
            CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            f->fault.begin=faults[i]=="begin"; f->fault.write=faults[i];
            BOOST_CHECK(!wallet.GetSaplingWallet().AddSpendingKey(sk,1)); Absent(wallet,sk);
        }
        auto reopened=Open(path); NoRecords(*reopened,sk);
    }
}
BOOST_AUTO_TEST_CASE(supplied_owner_abort_destructor_commit) {
    for (int mode=0;mode<3;++mode) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("owner-%d",mode));
        const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        {
            auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
            CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            {
                WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
                BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1,&batch));
                Absent(wallet,sk); BOOST_CHECK_EQUAL(f->fault.commits,0);
                if(mode==0) BOOST_REQUIRE(batch.TxnAbort());
                if(mode==2) BOOST_REQUIRE(batch.TxnCommit());
            }
            if(mode!=2) Absent(wallet,sk);
            else BOOST_CHECK(wallet.GetSaplingWallet().HaveSpendingKeyForPaymentAddress(sk.DefaultAddress()));
        }
        auto reopened=Open(path);
        if(mode!=2) NoRecords(*reopened,sk);
        else BOOST_CHECK(reopened->MakeBatch()->Exists(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key())));
    }
}
BOOST_AUTO_TEST_CASE(repeated_import_preserves_metadata) {
    const auto path=m_args.GetDataDirNet()/"repeat";
    const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    {
        CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path)); LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,42));
        BOOST_CHECK(!wallet.GetSaplingWallet().AddSpendingKey(sk,999));
        libzcash::SaplingExtendedSpendingKey loaded;
        BOOST_REQUIRE(wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk.DefaultAddress(),loaded));
        BOOST_CHECK(loaded==sk);
    }
    auto db=Open(path); CKeyMetadata meta;
    BOOST_REQUIRE(db->MakeBatch()->Read(std::make_pair(DBKeys::SAP_KEYMETA,sk.ToXFVK().fvk.in_viewing_key()),meta));
    BOOST_CHECK_EQUAL(meta.nCreateTime,42);
}
BOOST_AUTO_TEST_CASE(crypted_write_and_erase_matrix) {
    for(const auto& failure: {DBKeys::SAP_KEYMETA,DBKeys::SAP_KEY_CRIPTED,DBKeys::SAP_KEY}) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString(failure);
        const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        {
            auto db=std::make_unique<FaultDB>(Open(path));
            {
                WalletBatch batch(*db); BOOST_REQUIRE(batch.WriteSaplingZKey(sk.ToXFVK().fvk.in_viewing_key(),sk,CKeyMetadata(42)));
            }
            db->fault.write=failure; if(failure==DBKeys::SAP_KEY) db->fault.erase=failure;
            WalletBatch batch(*db);
            BOOST_CHECK(!batch.WriteCryptedSaplingZKey(sk.ToXFVK(),std::vector<unsigned char>(32,1),CKeyMetadata(999)));
        }
        auto db=Open(path); auto b=db->MakeBatch(); CKeyMetadata meta;
        BOOST_REQUIRE(b->Read(std::make_pair(DBKeys::SAP_KEYMETA,sk.ToXFVK().fvk.in_viewing_key()),meta));
        BOOST_CHECK_EQUAL(meta.nCreateTime,42);
        BOOST_CHECK(b->Exists(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key())));
        BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_KEY_CRIPTED,sk.ToXFVK())));
    }
}
BOOST_AUTO_TEST_CASE(locked_mixed_records_never_return_plaintext) {
    const auto path=m_args.GetDataDirNet()/"mixed";
    const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    {
        CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path)); LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1));
        BOOST_REQUIRE(wallet.EncryptWallet(SecureString("synthetic")));
        auto b=wallet.GetDatabase().MakeBatch();
        BOOST_REQUIRE(b->Write(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key()),sk));
    }
    CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path)); LOCK(wallet.cs_wallet);
    WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
    BOOST_REQUIRE(wallet.IsLocked()); libzcash::SaplingExtendedSpendingKey loaded;
    BOOST_CHECK(!wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk.DefaultAddress(),loaded));
    BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic")));
    BOOST_REQUIRE(wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk.DefaultAddress(),loaded));
    BOOST_CHECK(loaded==sk);
}
#ifndef WIN32
// Process-isolated fail-stop/reopen coverage. Native Windows needs an exec-based
// subprocess harness; the nonfatal persistence cases below are portable.
BOOST_AUTO_TEST_CASE(completion_uncertainty_is_terminal) {
    for(int mode=0;mode<4;++mode) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("uncertain-%d",mode));
        const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        pid_t pid=fork(); BOOST_REQUIRE(pid>=0);
        if(pid==0) {
            std::set_terminate([] { _exit(86); });
            auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
            CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            {
                WalletBatch batch(wallet.GetDatabase()); batch.TxnBegin();
                wallet.GetSaplingWallet().AddSpendingKey(sk,1,&batch);
                if(mode<2) { f->fault.commit_failure=mode+1; batch.TxnCommit(); }
                else { f->fault.abort_failure=true; if(mode==2) batch.TxnAbort(); }
            }
            _exit(0);
        }
        int status; BOOST_REQUIRE(waitpid(pid,&status,0)==pid);
        BOOST_CHECK(WIFEXITED(status) && WEXITSTATUS(status)==86);
        auto db=Open(path);
        if(mode!=1) NoRecords(*db,sk);
        else BOOST_CHECK(db->MakeBatch()->Exists(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key())));
    }
}
#endif
BOOST_AUTO_TEST_CASE(encryption_preflight_failure_preserves_wallet) {
    for(int mode=0;mode<2;++mode) {
        auto db=std::make_unique<FaultDB>(Open(m_args.GetDataDirNet()/fs::PathFromString(strprintf("enc-preflight-%d",mode))));
        auto* f=db.get(); CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
        const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1));
        f->fault.begin=mode==0; if(mode==1) f->fault.write=DBKeys::MASTER_KEY;
        BOOST_CHECK(!wallet.EncryptWallet(SecureString("synthetic")));
        BOOST_CHECK(!wallet.IsCrypted());
        libzcash::SaplingExtendedSpendingKey loaded;
        BOOST_REQUIRE(wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk.DefaultAddress(),loaded));
        BOOST_CHECK(loaded==sk);
    }
}
BOOST_AUTO_TEST_CASE(encrypted_add_failure_matrix) {
    for(const auto& failure: {DBKeys::SAP_KEYMETA,DBKeys::SAP_KEY_CRIPTED,DBKeys::SAP_ADDR}) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString("enc-add-"+failure);
        const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        {
            auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
            CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            BOOST_REQUIRE(wallet.EncryptWallet(SecureString("synthetic")));
            BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic")));
            f->fault.write=failure;
            BOOST_CHECK(!wallet.GetSaplingWallet().AddSpendingKey(sk,1)); Absent(wallet,sk);
        }
        auto db=Open(path); NoRecords(*db,sk);
    }
}
BOOST_AUTO_TEST_CASE(joined_failure_makes_transaction_rollback_only) {
    const auto path=m_args.GetDataDirNet()/"rollback-only";
    const auto a=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    const auto b=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    {
        auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
        CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
        WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(a,1,&batch));
        f->fault.write=DBKeys::SAP_ADDR;
        BOOST_CHECK(!wallet.GetSaplingWallet().AddSpendingKey(b,2,&batch));
        BOOST_CHECK(!batch.TxnCommit()); Absent(wallet,a); Absent(wallet,b);
    }
    auto db=Open(path); NoRecords(*db,a); NoRecords(*db,b);
}
BOOST_AUTO_TEST_CASE(sibling_compound_failures_are_atomic) {
    for(int mode=0;mode<5;++mode) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("sibling-%d",mode));
        CKey key; key.MakeNewKey(true); auto pub=key.GetPubKey();
        {
            auto db=std::make_unique<FaultDB>(Open(path)); WalletBatch batch(*db);
            if(mode==0) { db->fault.write=DBKeys::KEY; BOOST_CHECK(!batch.WriteKey(pub,key.GetPrivKey(),CKeyMetadata(42))); }
            if(mode==1) {
                BOOST_REQUIRE(batch.WriteKey(pub,key.GetPrivKey(),CKeyMetadata(42)));
                db->fault.erase=DBKeys::KEY;
                BOOST_CHECK(!batch.WriteCryptedKey(pub,std::vector<unsigned char>(32,1),CKeyMetadata(999)));
            }
            if(mode==2) {
                BOOST_REQUIRE(batch.WriteDescriptorKey(uint256{},pub,key.GetPrivKey(),SecureString{},SecureString{}));
                db->fault.erase=DBKeys::WALLETDESCRIPTORKEY;
                BOOST_CHECK(!batch.WriteCryptedDescriptorKey(uint256{},pub,std::vector<unsigned char>(32,1),{},{}));
            }
            if(mode==3) {
                CHDChain chain; BOOST_REQUIRE(batch.WriteHDChain(chain)); chain.SetCrypted(true);
                db->fault.erase=DBKeys::HDCHAIN; BOOST_CHECK(!batch.WriteHDChain(chain));
            }
            if(mode==4) {
                CHDPubKey hd; hd.extPubKey.pubkey=pub; db->fault.write=DBKeys::HDPUBKEY;
                BOOST_CHECK(!batch.WriteHDPubKey(hd,CKeyMetadata(42)));
            }
        }
        auto db=Open(path); auto b=db->MakeBatch();
        if(mode==0 || mode==4) BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::KEYMETA,pub)));
        if(mode==1) {
            CKeyMetadata meta; BOOST_REQUIRE(b->Read(std::make_pair(DBKeys::KEYMETA,pub),meta));
            BOOST_CHECK_EQUAL(meta.nCreateTime,42);
            BOOST_CHECK(b->Exists(std::make_pair(DBKeys::KEY,pub)));
            BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::CRYPTED_KEY,pub)));
        }
        if(mode==2) {
            BOOST_CHECK(b->Exists(std::make_pair(DBKeys::WALLETDESCRIPTORKEY,std::make_pair(uint256{},pub))));
            BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::WALLETDESCRIPTORCKEY,std::make_pair(uint256{},pub))));
        }
        if(mode==3) { BOOST_CHECK(b->Exists(DBKeys::HDCHAIN)); BOOST_CHECK(!b->Exists(DBKeys::CRYPTED_HDCHAIN)); }
    }
}
BOOST_AUTO_TEST_CASE(encryption_staging_abort_keeps_plain_key) {
    CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(m_args.GetDataDirNet()/"enc-stage")); LOCK(wallet.cs_wallet);
    const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1));
    WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
    CKeyingMaterial master(32,1); BOOST_REQUIRE(wallet.GetSaplingWallet().EncryptKeys(master,batch));
    BOOST_REQUIRE(batch.TxnAbort()); libzcash::SaplingExtendedSpendingKey loaded;
    BOOST_REQUIRE(wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk.DefaultAddress(),loaded));
    BOOST_CHECK(loaded==sk);
}
BOOST_AUTO_TEST_CASE(discovered_address_failure_does_not_publish) {
    const auto path = m_args.GetDataDirNet()/"address";
    const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    libzcash::diversifier_index_t index; index.begin()[0]=100;
    const auto alternate=sk.ToXFVK().Address(index); BOOST_REQUIRE(alternate);
    BOOST_REQUIRE(!(alternate->second==sk.DefaultAddress()));
    libzcash::SaplingNote note(alternate->second,10);
    libzcash::SaplingNotePlaintext plaintext(note,{}); const auto enc=plaintext.encrypt(note.pk_d); BOOST_REQUIRE(enc);
    OutputDescription output; output.cmu=*note.cmu();output.ephemeralKey=enc->second.get_epk();output.encCiphertext=enc->first;
    CMutableTransaction tx; tx.nVersion=CTransaction::SHIELDED_VERSION; tx.sapData.vShieldedOutput.push_back(output);
    auto ref = MakeTransactionRef(tx);
    {
        auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
        CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1));
        CWalletTx wtx(ref, TxStateInactive{}); f->fault.write=DBKeys::SAP_ADDR;
        BOOST_CHECK_THROW(wallet.GetSaplingWallet().ApplySaplingData(wtx), std::runtime_error);
        BOOST_CHECK(wtx.mapSaplingNoteData.empty());
        {
            WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
            BOOST_CHECK_THROW(wallet.GetSaplingWallet().ApplySaplingData(wtx, &batch), std::runtime_error);
            BOOST_CHECK(wtx.mapSaplingNoteData.empty());
            BOOST_CHECK(!batch.TxnCommit());
        }
        BOOST_CHECK(wallet.AddToWallet(ref, TxStateInactive{}, nullptr, false, false) == nullptr);
        BOOST_CHECK_EQUAL(wallet.mapWallet.count(ref->GetHash()), 0U);
        std::set<libzcash::SaplingPaymentAddress> addresses;wallet.GetSaplingWallet().GetPaymentAddresses(addresses);
        BOOST_CHECK_EQUAL(addresses.count(alternate->second),0U);
    }
    {
        auto db = Open(path); auto raw = db->MakeBatch();
        BOOST_CHECK(!raw->Exists(std::make_pair(DBKeys::TX, ref->GetHash())));
        BOOST_CHECK(!raw->Exists(std::make_pair(DBKeys::SAP_ADDR, alternate->second)));
    }
    // Retry after reopening must really persist, not take a false-success memory shortcut.
    {
        CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
        WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
        auto* result = wallet.AddToWallet(ref, TxStateInactive{}, nullptr, false, false);
        BOOST_REQUIRE(result); BOOST_CHECK(!result->mapSaplingNoteData.empty());
        BOOST_CHECK(!wallet.GetSaplingWallet().ApplySaplingData(*result)); // unchanged is not failure
    }
    auto db = Open(path); auto raw = db->MakeBatch();
    BOOST_CHECK(raw->Exists(std::make_pair(DBKeys::TX, ref->GetHash())));
    BOOST_CHECK(raw->Exists(std::make_pair(DBKeys::SAP_ADDR, alternate->second)));
}
#ifndef WIN32
BOOST_AUTO_TEST_CASE(encryption_faults_reopen_complete_original_wallet) {
    m_args.ForceSetArg("-keypool","1");
    for(int mode=0;mode<7;++mode) {
        const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("encrypt-fault-%d",mode));
        CKey key1,key2;key1.MakeNewKey(true);key2.MakeNewKey(true);
        const auto sk1=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        const auto sk2=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        pid_t pid=fork(); BOOST_REQUIRE(pid>=0);
        if(pid==0) {
            std::set_terminate([] { _exit(86); });
            auto db=std::make_unique<FaultDB>(Open(path));auto* f=db.get();
            CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            wallet.SetupLegacyScriptPubKeyMan();auto* man=wallet.GetLegacyScriptPubKeyMan();
            if(!man->AddKeyPubKey(key1,key1.GetPubKey()) || !man->AddKeyPubKey(key2,key2.GetPubKey()) ||
               !wallet.GetSaplingWallet().AddSpendingKey(sk1,1) || !wallet.GetSaplingWallet().AddSpendingKey(sk2,2)) _exit(90);
            if(mode==0) f->fault.write=DBKeys::KEYMETA;
            if(mode==1) f->fault.write=DBKeys::CRYPTED_KEY;
            if(mode==2) f->fault.erase=DBKeys::KEY;
            if(mode==3) f->fault.write=DBKeys::SAP_KEYMETA;
            if(mode==4) f->fault.write=DBKeys::SAP_KEY_CRIPTED;
            if(mode==5) f->fault.erase=DBKeys::SAP_KEY;
            if(mode==6) f->fault.commit_failure=1;
            wallet.EncryptWallet(SecureString("synthetic"));
            _exit(0);
        }
        int status;BOOST_REQUIRE(waitpid(pid,&status,0)==pid);
        BOOST_CHECK(WIFEXITED(status) && WEXITSTATUS(status)==86);
        auto db=Open(path);auto b=db->MakeBatch();
        for(const auto& key:{key1,key2}) {
            BOOST_CHECK(b->Exists(std::make_pair(DBKeys::KEY,key.GetPubKey())));
            BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::CRYPTED_KEY,key.GetPubKey())));
        }
        for(const auto& sk:{sk1,sk2}) {
            BOOST_CHECK(b->Exists(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key())));
            BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_KEY_CRIPTED,sk.ToXFVK())));
        }
        BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::MASTER_KEY,1U)));
    }
}
#endif
BOOST_AUTO_TEST_CASE(multiple_staged_additions_publish_only_after_commit) {
    CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(m_args.GetDataDirNet()/"multiple"));LOCK(wallet.cs_wallet);
    const auto a=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    const auto b=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(a,1,&batch));
    BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(b,2,&batch));
    Absent(wallet,a);Absent(wallet,b);BOOST_REQUIRE(batch.TxnCommit());
    BOOST_CHECK(wallet.GetSaplingWallet().HaveSpendingKeyForPaymentAddress(a.DefaultAddress()));
    BOOST_CHECK(wallet.GetSaplingWallet().HaveSpendingKeyForPaymentAddress(b.DefaultAddress()));
}
BOOST_AUTO_TEST_CASE(destroyed_service_discards_publication) {
    CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(m_args.GetDataDirNet()/"lifetime"));LOCK(wallet.cs_wallet);
    WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
    const auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    auto service=MakeSaplingService(wallet);
    BOOST_REQUIRE(service->AddSpendingKey(sk,1,&batch)); service.reset();
    BOOST_REQUIRE(batch.TxnCommit());
    BOOST_CHECK(wallet.GetDatabase().MakeBatch()->Exists(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key())));
}
BOOST_AUTO_TEST_CASE(successful_encryption_reopens_without_plain_records) {
    m_args.ForceSetArg("-keypool","1");
    const auto path=m_args.GetDataDirNet()/"enc-success";
    CKey key1,key2;key1.MakeNewKey(true);key2.MakeNewKey(true);
    const auto sk1=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    const auto sk2=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
    {
        CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path));LOCK(wallet.cs_wallet);
        wallet.SetupLegacyScriptPubKeyMan();auto* man=wallet.GetLegacyScriptPubKeyMan();
        BOOST_REQUIRE(man->AddKeyPubKey(key1,key1.GetPubKey())); BOOST_REQUIRE(man->AddKeyPubKey(key2,key2.GetPubKey()));
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk1,1));BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk2,2));
        BOOST_REQUIRE(wallet.EncryptWallet(SecureString("synthetic")));
        BOOST_REQUIRE(wallet.IsLocked());BOOST_CHECK(!wallet.Unlock(SecureString("wrong")));
        auto b=wallet.GetDatabase().MakeBatch();
        for(const auto& key:{key1,key2}) {
            BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::KEY,key.GetPubKey())));
            BOOST_CHECK(b->Exists(std::make_pair(DBKeys::CRYPTED_KEY,key.GetPubKey())));
        }
        for(const auto& sk:{sk1,sk2}) {
            BOOST_CHECK(!b->Exists(std::make_pair(DBKeys::SAP_KEY,sk.ToXFVK().fvk.in_viewing_key())));
            BOOST_CHECK(b->Exists(std::make_pair(DBKeys::SAP_KEY_CRIPTED,sk.ToXFVK())));
        }
    }
    CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path));LOCK(wallet.cs_wallet);
    WalletBatch batch(wallet.GetDatabase());BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
    BOOST_REQUIRE(wallet.IsLocked());libzcash::SaplingExtendedSpendingKey loaded;
    BOOST_CHECK(!wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk1.DefaultAddress(),loaded));
    BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic")));
    for(const auto& sk:{sk1,sk2}) {
        BOOST_REQUIRE(wallet.GetSaplingWallet().GetSpendingKeyForPaymentAddress(sk.DefaultAddress(),loaded));BOOST_CHECK(loaded==sk);
    }
    for(const auto& key:{key1,key2}) {
        CKey recovered;BOOST_REQUIRE(wallet.GetLegacyScriptPubKeyMan()->GetKey(key.GetPubKey().GetID(),recovered));BOOST_CHECK(recovered==key);
    }
}
BOOST_AUTO_TEST_CASE(conflicting_crypted_key_preserves_existing_record) {
    const auto path=m_args.GetDataDirNet()/"crypted-conflict";
    CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey();
    const std::vector<unsigned char> original(32,1), conflicting(32,2);
    {
        auto db=Open(path); WalletBatch batch(*db);
        BOOST_REQUIRE(batch.WriteCryptedKey(pub,original,CKeyMetadata(42)));
        BOOST_CHECK(!batch.WriteCryptedKey(pub,conflicting,CKeyMetadata(999)));
    }
    auto db=Open(path);auto b=db->MakeBatch(); CKeyMetadata meta;
    BOOST_REQUIRE(b->Read(std::make_pair(DBKeys::KEYMETA,pub),meta)); BOOST_CHECK_EQUAL(meta.nCreateTime,42);
    std::pair<std::vector<unsigned char>,uint256> record;
    BOOST_REQUIRE(b->Read(std::make_pair(DBKeys::CRYPTED_KEY,pub),record));
    BOOST_CHECK(record.first==original);BOOST_CHECK(record.second==Hash(original));
}
#ifdef USE_SQLITE
BOOST_AUTO_TEST_CASE(sqlite_supplied_batch_preserves_owner_completion) {
    for (bool address_only : {false, true}) for (int mode = 0; mode < 3; ++mode) {
        const auto path = m_args.GetDataDirNet()/fs::PathFromString(strprintf("sqlite-owner-%d-%d", address_only, mode));
        const auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        libzcash::diversifier_index_t index; index.begin()[0] = 100;
        const auto alternate = sk.ToXFVK().Address(index); BOOST_REQUIRE(alternate);
        const auto address = address_only ? alternate->second : sk.DefaultAddress();
        {
            DatabaseOptions options; DatabaseStatus status; bilingual_str error;
            auto db = MakeSQLiteDatabase(path, options, status, error); BOOST_REQUIRE_MESSAGE(db, error.original);
            auto* sqlite = db.get();
            CWallet wallet(nullptr, nullptr, "synthetic", m_args, std::move(db)); LOCK(wallet.cs_wallet);
            if (address_only) BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk, 1));
            {
                WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
                if (address_only) {
                    libzcash::SaplingNote note(address, 10);
                    libzcash::SaplingNotePlaintext plaintext(note, {});
                    const auto enc = plaintext.encrypt(note.pk_d); BOOST_REQUIRE(enc);
                    OutputDescription output; output.cmu = *note.cmu(); output.ephemeralKey = enc->second.get_epk(); output.encCiphertext = enc->first;
                    CMutableTransaction tx; tx.nVersion = CTransaction::SHIELDED_VERSION; tx.sapData.vShieldedOutput.push_back(output);
                    CWalletTx wtx(MakeTransactionRef(tx), TxStateInactive{});
                    BOOST_REQUIRE(wallet.GetSaplingWallet().ApplySaplingData(wtx, &batch));
                } else BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk, 1, &batch));
                // Inspect the connection itself: a temporary SQLite batch would abort it.
                BOOST_CHECK_EQUAL(sqlite3_get_autocommit(sqlite->m_db), 0);
                std::set<libzcash::SaplingPaymentAddress> addresses;
                wallet.GetSaplingWallet().GetPaymentAddresses(addresses);
                BOOST_CHECK_EQUAL(addresses.count(address), 0U);
                if (mode == 0) BOOST_REQUIRE(batch.TxnCommit());
                if (mode == 1) BOOST_REQUIRE(batch.TxnAbort());
            } // mode 2: owner destructor aborts.
            std::set<libzcash::SaplingPaymentAddress> addresses;
            wallet.GetSaplingWallet().GetPaymentAddresses(addresses);
            BOOST_CHECK_EQUAL(addresses.count(address), mode == 0 ? 1U : 0U);
        } // Normal wallet/database close before reopening; no forked SQLite state.
        DatabaseOptions options; DatabaseStatus db_status; bilingual_str error;
        auto reopened = MakeSQLiteDatabase(path, options, db_status, error); BOOST_REQUIRE_MESSAGE(reopened, error.original);
        auto raw = reopened->MakeBatch();
        BOOST_CHECK_EQUAL(raw->Exists(std::make_pair(DBKeys::SAP_ADDR, address)), mode == 0);
        BOOST_CHECK_EQUAL(raw->Exists(std::make_pair(DBKeys::SAP_KEY, sk.ToXFVK().fvk.in_viewing_key())), address_only || mode == 0);
    }
}
#endif
namespace {
CTransactionRef ReviewTx(const libzcash::SaplingExtendedSpendingKey& sk, unsigned count = 2) {
    CMutableTransaction tx; tx.nVersion = CTransaction::SHIELDED_VERSION;
    libzcash::diversifier_index_t index; index.begin()[0] = 100;
    for (unsigned i = 0; i < count; ++i) {
        auto a = sk.ToXFVK().Address(index); BOOST_REQUIRE(a);
        index = a->first; ++index.begin()[0];
        libzcash::SaplingNote note(a->second, 10);
        libzcash::SaplingNotePlaintext plain(note, {}); auto enc = plain.encrypt(note.pk_d); BOOST_REQUIRE(enc);
        OutputDescription o; o.cmu = *note.cmu(); o.ephemeralKey = enc->second.get_epk(); o.encCiphertext = enc->first;
        tx.sapData.vShieldedOutput.push_back(o);
    }
    return MakeTransactionRef(tx);
}
std::unique_ptr<WalletDatabase> ReviewOpen(const fs::path& path, bool sqlite) {
    if (!sqlite) return Open(path);
#ifdef USE_SQLITE
    DatabaseOptions options; DatabaseStatus status; bilingual_str error;
    auto db = MakeSQLiteDatabase(path, options, status, error); BOOST_REQUIRE_MESSAGE(db,error.original); return db;
#else
    BOOST_FAIL("SQLite backend not compiled"); return {};
#endif
}
const std::vector<bool> RESCAN_BACKENDS{false
#ifdef USE_SQLITE
    , true
#endif
};
}
BOOST_AUTO_TEST_CASE(rescan_failure_preserves_complete_previous_state) {
    for (bool sqlite : RESCAN_BACKENDS) {
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        auto sk2 = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        CMutableTransaction tx(*ReviewTx(sk, 1));
        tx.sapData.vShieldedOutput.push_back(ReviewTx(sk2, 1)->sapData.vShieldedOutput.front());
        auto ref = MakeTransactionRef(tx);
        auto db = std::make_unique<FaultDB>(ReviewOpen(m_args.GetDataDirNet()/fs::PathFromString(strprintf("rescan-%d", sqlite)), sqlite));
        auto* f = db.get();
        CWallet wallet(nullptr, nullptr, "rescan", m_args, std::move(db)); LOCK(wallet.cs_wallet);
        auto& service = wallet.GetSaplingWallet();
        BOOST_REQUIRE(service.AddSpendingKey(sk, 1));
        auto* wtx = wallet.AddToWallet(ref, TxStateInactive{}, nullptr, false, false); BOOST_REQUIRE(wtx);
        // Actual import A/decryption/publication, not a manually seeded note.
        BOOST_REQUIRE_EQUAL(wtx->mapSaplingNoteData.size(), 1U);
        // Synthetic witness/index fixtures stand in for a previous chain rebuild.
        auto& nd = wtx->mapSaplingNoteData.begin()->second;
        SaplingMerkleTree tree; tree.append(ref->sapData.vShieldedOutput.front().cmu);
        nd.witnesses.push_front(tree.witness()); nd.witnessHeight = 42; nd.nullifier = uint256S("01");
        SaplingWalletTestAccess::Nullifiers(service)[*nd.nullifier] = wtx->mapSaplingNoteData.begin()->first;
        service.AddToSaplingSpends(*nd.nullifier, ref->GetHash());
        // Also protect another transaction that may have been scanned first.
        auto* other = wallet.AddToWallet(ReviewTx(sk, 2), TxStateInactive{}, nullptr, false, false); BOOST_REQUIRE(other);
        // Preserve auxiliary data on every existing transaction, including any
        // visited before the failing one (mapWallet traversal is unordered).
        for (auto& [op, data] : other->mapSaplingNoteData) {
            data.witnesses.push_front(tree.witness()); data.witnessHeight = 41;
        }
        const auto old_notes = wtx->mapSaplingNoteData;
        const auto other_notes = other->mapSaplingNoteData;
        const auto old_spends = SaplingWalletTestAccess::Spends(service);
        const auto old_nullifiers = SaplingWalletTestAccess::Nullifiers(service);
        BOOST_REQUIRE(service.AddSpendingKey(sk2, 1));
        f->fault.write = DBKeys::SAP_ADDR;
        BOOST_CHECK_THROW(service.RescanWalletTransactions(), std::runtime_error);
        BOOST_CHECK(wtx->mapSaplingNoteData == old_notes);
        BOOST_CHECK(other->mapSaplingNoteData == other_notes);
        BOOST_CHECK(SaplingWalletTestAccess::Spends(service) == old_spends);
        BOOST_CHECK(SaplingWalletTestAccess::Nullifiers(service) == old_nullifiers);
        if (!wtx->mapSaplingNoteData.empty()) {
            CDataStream before(SER_DISK, CLIENT_VERSION), after(SER_DISK, CLIENT_VERSION);
            before << old_notes.begin()->second.witnesses;
            after << wtx->mapSaplingNoteData.begin()->second.witnesses;
            BOOST_CHECK(before.str() == after.str());
        }
        for (const auto& [op, data] : other->mapSaplingNoteData) {
            CDataStream before(SER_DISK, CLIENT_VERSION), after(SER_DISK, CLIENT_VERSION);
            before << other_notes.at(op).witnesses; after << data.witnesses;
            BOOST_CHECK(before.str() == after.str());
        }
        // Successful rebuild must replace, not merge stale notes/index entries.
        wtx->mapSaplingNoteData.emplace(SaplingOutPoint{ref->GetHash(), 99}, old_notes.begin()->second);
        f->fault.write.clear();
        BOOST_CHECK_NO_THROW(service.RescanWalletTransactions());
        BOOST_CHECK_EQUAL(wtx->mapSaplingNoteData.size(), 2U);
        BOOST_CHECK_EQUAL(wtx->mapSaplingNoteData.count(SaplingOutPoint{ref->GetHash(), 99}), 0U);
        BOOST_CHECK(SaplingWalletTestAccess::Spends(service).empty());
        BOOST_CHECK(SaplingWalletTestAccess::Nullifiers(service).empty());
        for (const auto& [op, data] : wtx->mapSaplingNoteData) {
            BOOST_CHECK(data.witnesses.empty()); BOOST_CHECK_EQUAL(data.witnessHeight, -1); BOOST_CHECK(!data.nullifier);
        }
    }
}
// A joined caller owns a provisional CWalletTx, never a live mapWallet entry.
// It publishes that value only after commit, and discards it on abort/failure.
BOOST_AUTO_TEST_CASE(joined_caller_staging_defers_global_spends) {
    for (bool sqlite : RESCAN_BACKENDS) for (bool known_address : {false, true}) for (int mode = 0; mode < 4; ++mode) {
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        CMutableTransaction tx(*ReviewTx(sk, 1));
        SpendDescription spend; spend.nullifier = uint256S("02"); tx.sapData.vShieldedSpend.push_back(spend);
        auto ref = MakeTransactionRef(tx);
        auto db = std::make_unique<FaultDB>(ReviewOpen(m_args.GetDataDirNet()/fs::PathFromString(strprintf("staging-%d-%d-%d", sqlite, known_address, mode)), sqlite));
        auto* f = db.get();
        CWallet wallet(nullptr, nullptr, "staging", m_args, std::move(db)); LOCK(wallet.cs_wallet);
        auto& service = wallet.GetSaplingWallet(); BOOST_REQUIRE(service.AddSpendingKey(sk, 1));
        if (known_address) {
            CWalletTx output_only(ReviewTx(sk, 1), TxStateInactive{});
            BOOST_REQUIRE(service.ApplySaplingData(output_only));
        }
        CWalletTx staged(ref, TxStateInactive{}); // Retained through owner completion.
        {
            WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
            BOOST_REQUIRE(service.ApplySaplingData(staged, &batch));
            BOOST_CHECK_EQUAL(staged.mapSaplingNoteData.size(), 1U); // provisional only
            BOOST_CHECK(wallet.mapWallet.empty());
            BOOST_CHECK(SaplingWalletTestAccess::Spends(service).empty());
            if (mode == 0) {
                BOOST_REQUIRE(batch.TxnCommit());
                auto [it, inserted] = wallet.mapWallet.try_emplace(ref->GetHash(), ref, TxStateInactive{});
                BOOST_REQUIRE(inserted);
                it->second.mapSaplingNoteData.swap(staged.mapSaplingNoteData);
            } else if (mode == 1) {
                BOOST_REQUIRE(batch.TxnAbort());
            } else if (mode == 3) {
                // A later operation poisons this owner's entire transaction.
                f->fault.write = DBKeys::SAP_ADDR;
                CWalletTx failed(ReviewTx(sk, 2), TxStateInactive{});
                BOOST_CHECK_THROW(service.ApplySaplingData(failed, &batch), std::runtime_error);
                BOOST_CHECK(!batch.TxnCommit());
            }
        } // mode 2: owner destructor aborts, staged is still alive.
        BOOST_CHECK_EQUAL(SaplingWalletTestAccess::Spends(service).size(), mode == 0 ? 1U : 0U);
        BOOST_CHECK_EQUAL(wallet.mapWallet.size(), mode == 0 ? 1U : 0U);
        if (mode != 0) {
            BOOST_CHECK_EQUAL(staged.mapSaplingNoteData.size(), 1U);
            staged.mapSaplingNoteData.clear(); // Caller discards its provisional result.
        }
    }
}
BOOST_AUTO_TEST_CASE(joined_spend_callback_owns_identity_not_caller_reference) {
    for (bool sqlite : RESCAN_BACKENDS) for (bool destroy_service : {false, true}) {
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        CMutableTransaction tx(*ReviewTx(sk, 1));
        SpendDescription spend; spend.nullifier = uint256S("03"); tx.sapData.vShieldedSpend.push_back(spend);
        CWallet wallet(nullptr, nullptr, "lifetime", m_args, ReviewOpen(m_args.GetDataDirNet()/fs::PathFromString(strprintf("spend-lifetime-%d-%d", sqlite, destroy_service)), sqlite)); LOCK(wallet.cs_wallet);
        auto owned = MakeSaplingService(wallet);
        auto& service = destroy_service ? *owned : wallet.GetSaplingWallet();
        BOOST_REQUIRE(service.AddSpendingKey(sk, 1));
        WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
        const auto ref = MakeTransactionRef(tx);
        {
            CWalletTx temporary(ref, TxStateInactive{});
            BOOST_REQUIRE(service.ApplySaplingData(temporary, &batch));
        } // No completion callback may retain temporary by reference.
        if (destroy_service) owned.reset();
        BOOST_REQUIRE(batch.TxnCommit());
        const auto& spends = SaplingWalletTestAccess::Spends(wallet.GetSaplingWallet());
        BOOST_CHECK_EQUAL(spends.size(), destroy_service ? 0U : 1U);
        if (!destroy_service) BOOST_CHECK(spends.begin()->second == ref->GetHash());
    }
}
// Storage failures intentionally escape SyncTransaction, just like generic TX
// write failures. RPC/startup catches are not protection for async callbacks.
BOOST_AUTO_TEST_CASE(notification_storage_failure_uses_generic_error_boundary) {
    for (bool sqlite : RESCAN_BACKENDS) for (const auto& tag : {DBKeys::SAP_ADDR, DBKeys::TX}) for (bool disconnect : {false, true}) {
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32)); auto ref = ReviewTx(sk, 1);
        auto db = std::make_unique<FaultDB>(ReviewOpen(m_args.GetDataDirNet()/fs::PathFromString(strprintf("notification-%d-%s-%d", sqlite, tag, disconnect)), sqlite));
        auto* f = db.get();
        CWallet wallet(nullptr, nullptr, "notification", m_args, std::move(db)); LOCK(wallet.cs_wallet);
        BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk, 1)); f->fault.write = tag;
        auto notify = [&] {
            if (disconnect) { CBlock block; block.vtx.push_back(ref); wallet.blockDisconnected(block, 10); }
            else wallet.transactionAddedToMempool(ref, 0);
        };
        BOOST_CHECK_EXCEPTION(notify(), std::runtime_error, [](const std::runtime_error& e) {
            return std::string(e.what()) == "DB error adding transaction to wallet, write failed";
        });
        if (tag == DBKeys::SAP_ADDR) BOOST_CHECK(wallet.mapWallet.empty());
    }
}
#ifndef WIN32
BOOST_AUTO_TEST_CASE(uncaught_notification_error_is_fail_stop) {
    // Actual thread boundary, process-isolated so expected termination cannot
    // kill the suite. Not an end-to-end validation scheduler/node test.
    for (const auto& tag : {DBKeys::SAP_ADDR, DBKeys::TX}) {
        const auto path = m_args.GetDataDirNet()/fs::PathFromString("async-" + tag);
        pid_t pid = fork(); BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            std::set_terminate([] { _exit(86); });
            std::thread callback([&] {
                auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32)); auto ref = ReviewTx(sk, 1);
                auto db = std::make_unique<FaultDB>(Open(path)); auto* f = db.get();
                CWallet wallet(nullptr, nullptr, "async", m_args, std::move(db));
                {
                    LOCK(wallet.cs_wallet);
                    if (!wallet.GetSaplingWallet().AddSpendingKey(sk, 1)) _exit(90);
                    f->fault.write = tag;
                }
                wallet.transactionAddedToMempool(ref, 0); // Deliberately uncaught.
                _exit(91);
            });
            callback.join(); _exit(92);
        }
        int status; BOOST_REQUIRE(waitpid(pid, &status, 0) == pid);
        BOOST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 86);
    }
}
#endif
BOOST_AUTO_TEST_CASE(review_partial_second_address_failure) {
    for (bool sqlite : RESCAN_BACKENDS) for (bool joined : {false,true}) {
        auto path = m_args.GetDataDirNet()/fs::PathFromString(strprintf("partial-%d-%d",sqlite,joined));
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32)); auto ref = ReviewTx(sk);
        std::vector<libzcash::SaplingPaymentAddress> discovered;
        {
            auto db = std::make_unique<FaultDB>(ReviewOpen(path,sqlite)); auto* f = db.get();
            CWallet wallet(nullptr,nullptr,"review",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1));
            auto data = wallet.GetSaplingWallet().FindMySaplingNotes(*ref); BOOST_REQUIRE_EQUAL(data.second.size(),2U);
            for (const auto& [a,k] : data.second) discovered.push_back(a);
            f->fault.write = DBKeys::SAP_ADDR; f->fault.fail_nth = 2;
            CWalletTx wtx(ref,TxStateInactive{});
            {
                WalletBatch batch(wallet.GetDatabase()); if(joined) BOOST_REQUIRE(batch.TxnBegin());
                BOOST_CHECK_THROW(wallet.GetSaplingWallet().ApplySaplingData(wtx,&batch),std::runtime_error);
                BOOST_CHECK_EQUAL(f->fault.matched,2);
                BOOST_CHECK(wtx.mapSaplingNoteData.empty());
                if(joined) BOOST_CHECK(!batch.TxnCommit());
            }
            std::set<libzcash::SaplingPaymentAddress> addresses; wallet.GetSaplingWallet().GetPaymentAddresses(addresses);
            for (const auto& a: discovered) BOOST_CHECK_EQUAL(addresses.count(a),0U);
        }
        auto db=ReviewOpen(path,sqlite); auto raw=db->MakeBatch();
        for (const auto& a: discovered) BOOST_CHECK(!raw->Exists(std::make_pair(DBKeys::SAP_ADDR,a)));
    }
}
BOOST_AUTO_TEST_CASE(review_existing_transaction_failure_and_retry) {
    for(bool sqlite : RESCAN_BACKENDS) {
        auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("existing-%d",sqlite));
        auto sk=libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32)); auto ref=ReviewTx(sk);
        {
            auto db=std::make_unique<FaultDB>(ReviewOpen(path,sqlite)); auto* f=db.get();
            CWallet wallet(nullptr,nullptr,"review",m_args,std::move(db)); LOCK(wallet.cs_wallet);
            auto* original=wallet.AddToWallet(ref,TxStateInactive{},nullptr,false,false); BOOST_REQUIRE(original);
            auto order=original->nOrderPos; auto time=original->nTimeReceived;
            BOOST_REQUIRE(wallet.GetSaplingWallet().AddSpendingKey(sk,1));
            f->fault.write=DBKeys::SAP_ADDR; f->fault.fail_nth=2;
            bool called=false;
            BOOST_CHECK(wallet.AddToWallet(ref,TxStateInMempool{},[&](CWalletTx&,bool){called=true;return true;},false,false)==nullptr);
            BOOST_CHECK(!called); BOOST_REQUIRE_EQUAL(wallet.mapWallet.size(),1U);
            BOOST_CHECK(original->state<TxStateInactive>()); BOOST_CHECK(original->mapSaplingNoteData.empty());
            BOOST_CHECK_EQUAL(original->nOrderPos,order); BOOST_CHECK_EQUAL(original->nTimeReceived,time);
        }
        {
            CWallet wallet(nullptr,nullptr,"review",m_args,ReviewOpen(path,sqlite)); LOCK(wallet.cs_wallet);
            WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
            auto* old=wallet.GetWalletTx(ref->GetHash()); BOOST_REQUIRE(old); BOOST_CHECK(old->state<TxStateInactive>());
            BOOST_CHECK(old->mapSaplingNoteData.empty());
            auto* retried=wallet.AddToWallet(ref,TxStateInMempool{},nullptr,false,false); BOOST_REQUIRE(retried);
            BOOST_CHECK_EQUAL(retried->mapSaplingNoteData.size(),2U);
        }
        auto db=ReviewOpen(path,sqlite); auto raw=db->MakeBatch(); BOOST_CHECK(raw->Exists(std::make_pair(DBKeys::TX,ref->GetHash())));
    }
}
BOOST_AUTO_TEST_CASE(owned_spend_publication_with_fresh_batch) {
    for (bool sqlite : RESCAN_BACKENDS) for (bool supplied : {false, true}) for (bool with_output : {false, true}) {
        auto sk = libzcash::SaplingExtendedSpendingKey::Master(HDSeed::Random(32));
        CMutableTransaction tx;
        tx.nVersion = CTransaction::SHIELDED_VERSION;
        if (with_output) tx.sapData.vShieldedOutput = ReviewTx(sk, 1)->sapData.vShieldedOutput;
        SpendDescription spend; spend.nullifier = uint256S("04"); tx.sapData.vShieldedSpend.push_back(spend);
        auto db = std::make_unique<FaultDB>(ReviewOpen(m_args.GetDataDirNet()/fs::PathFromString(strprintf("owned-spend-%d-%d-%d", sqlite, supplied, with_output)), sqlite));
        auto* f = db.get();
        CWallet wallet(nullptr, nullptr, "owned-spend", m_args, std::move(db)); LOCK(wallet.cs_wallet);
        auto& service = wallet.GetSaplingWallet(); BOOST_REQUIRE(service.AddSpendingKey(sk, 1));
        CWalletTx staged(MakeTransactionRef(tx), TxStateInactive{});
        f->fault.begin = true;
        BOOST_CHECK_THROW(service.ApplySaplingData(staged), std::runtime_error);
        BOOST_CHECK(staged.mapSaplingNoteData.empty());
        BOOST_CHECK(SaplingWalletTestAccess::Spends(service).empty());
        f->fault.begin = false;
        if (supplied) {
            WalletBatch batch(wallet.GetDatabase());
            BOOST_CHECK_EQUAL(service.ApplySaplingData(staged, &batch), with_output);
        } else BOOST_CHECK_EQUAL(service.ApplySaplingData(staged), with_output);
        BOOST_CHECK_EQUAL(SaplingWalletTestAccess::Spends(service).size(), 1U);
        BOOST_CHECK_EQUAL(staged.mapSaplingNoteData.size(), with_output ? 1U : 0U);
    }
}
BOOST_AUTO_TEST_SUITE_END()
}
