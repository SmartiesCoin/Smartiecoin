// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>
#include <wallet/bdb.h>
#ifdef USE_SQLITE
#include <wallet/sqlite.h>
#include <sqlite3.h>
#endif
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <boost/test/unit_test.hpp>
#include <db_cxx.h>
#ifndef WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wallet {
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
};
std::string Tag(const CDataStream& s) { auto copy = s; std::string tag; copy >> tag; return tag; }
class FaultBatch final : public DatabaseBatch {
    std::unique_ptr<DatabaseBatch> inner;
    Fault& f;
    bool ReadKey(CDataStream&& k, CDataStream& v) override { Raw out{v}; return inner->Read(Raw{k}, out); }
    bool WriteKey(CDataStream&& k, CDataStream&& v, bool overwrite) override {
        return Tag(k) != f.write && inner->Write(Raw{k}, Raw{v}, overwrite);
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
}
BOOST_FIXTURE_TEST_SUITE(transparent_storage_safety, BasicTestingSetup)
BOOST_AUTO_TEST_CASE(pinned_backend_version) {
    int major,minor; DbEnv::version(&major,&minor,nullptr);
    BOOST_CHECK_EQUAL(major,4); BOOST_CHECK_EQUAL(minor,8);
}
BOOST_AUTO_TEST_CASE(failed_import_retry_survives_reopen) {
 const auto path=m_args.GetDataDirNet()/"review-transparent";
 CKey key; key.MakeNewKey(true); auto pub=key.GetPubKey(); auto id=pub.GetID();
 {
  auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
  wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
  std::map<CKeyID,CKey> keys{{id,key}};
  f->fault.write=DBKeys::KEY;
  BOOST_REQUIRE(!man->ImportPrivKeys(keys,42));
  BOOST_CHECK(!man->HaveKey(id));
  f->fault.write.clear();
  BOOST_REQUIRE(man->ImportPrivKeys(keys,42));
  BOOST_CHECK(wallet.GetDatabase().MakeBatch()->Exists(std::make_pair(DBKeys::KEY,pub)));
 }
 CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
 WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
 auto* man=wallet.GetLegacyScriptPubKeyMan();
 BOOST_REQUIRE(man); BOOST_CHECK(man->HaveKey(id));
}
// No fault double: BDB's DB_NOOVERWRITE rejection of orphan metadata must
// remain a failure on retry, preserving both the old record and watch tracking.
BOOST_AUTO_TEST_CASE(real_bdb_orphan_metadata_preserves_watchonly) {
 const auto path=m_args.GetDataDirNet()/"orphan-metadata";
 CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey(); const auto id=pub.GetID();
 const auto script=GetScriptForDestination(PKHash(pub));
 {
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path)); LOCK(wallet.cs_wallet);
  wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
  BOOST_REQUIRE(man->AddWatchOnly(script,42));
  { auto raw=wallet.GetDatabase().MakeBatch(); BOOST_REQUIRE(raw->Write(std::make_pair(DBKeys::KEYMETA,pub),CKeyMetadata(17))); }
  const auto metadata_before=man->mapKeyMetadata;
  const auto birthday_before=man->GetTimeFirstKey();
  for(int attempt=0;attempt<2;++attempt) {
   BOOST_CHECK(!man->ImportPrivKeys({{id,key}},99));
   BOOST_CHECK(!man->HaveKey(id)); BOOST_CHECK(man->HaveWatchOnly(script));
   BOOST_CHECK_EQUAL(man->mapKeyMetadata.count(id),metadata_before.count(id));
   BOOST_CHECK_EQUAL(man->GetTimeFirstKey(),birthday_before);
  }
 }
 CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
 WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
 auto* man=wallet.GetLegacyScriptPubKeyMan(); BOOST_REQUIRE(man);
 BOOST_CHECK(!man->HaveKey(id)); BOOST_CHECK(man->HaveWatchOnly(script));
 CKeyMetadata meta; auto raw=wallet.GetDatabase().MakeBatch();
 BOOST_REQUIRE(raw->Read(std::make_pair(DBKeys::KEYMETA,pub),meta)); BOOST_CHECK_EQUAL(meta.nCreateTime,17);
 BOOST_CHECK(!raw->Exists(std::make_pair(DBKeys::KEY,pub)));
}
BOOST_AUTO_TEST_CASE(supplied_owner_controls_publication) {
 for(int mode=0;mode<3;++mode) {
  const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("owner-%d",mode));
  CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey();
  const auto script=GetScriptForRawPubKey(pub);
  {
   CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path)); LOCK(wallet.cs_wallet);
   wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
   BOOST_REQUIRE(man->AddWatchOnly(script,42));
   {
    WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(man->AddKeyPubKeyWithDB(batch,key,pub));
    BOOST_CHECK(!man->HaveKey(pub.GetID())); BOOST_CHECK(man->HaveWatchOnly(script));
    if(mode==0) BOOST_REQUIRE(batch.TxnAbort());
    if(mode==2) BOOST_REQUIRE(batch.TxnCommit());
   }
   BOOST_CHECK_EQUAL(man->HaveKey(pub.GetID()),mode==2);
   BOOST_CHECK_EQUAL(man->HaveWatchOnly(script),mode!=2);
  }
  CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
  WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
  auto* man=wallet.GetLegacyScriptPubKeyMan();
  if (!man) { BOOST_ERROR("Reopened wallet lost all legacy key/watch records"); continue; }
  BOOST_CHECK_EQUAL(man->HaveKey(pub.GetID()),mode==2);
  BOOST_CHECK_EQUAL(man->HaveWatchOnly(script),mode!=2);
 }
}
BOOST_AUTO_TEST_CASE(existing_key_import_is_idempotent) {
 const auto path=m_args.GetDataDirNet()/"existing-key";
 CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey(); const auto id=pub.GetID();
 {
  auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
  wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
  BOOST_REQUIRE(man->ImportPrivKeys({{id,key}},42));
  const auto birthday=man->GetTimeFirstKey();
  f->fault.write=DBKeys::KEY;
  BOOST_REQUIRE(man->ImportPrivKeys({{id,key}},99));
  BOOST_CHECK(man->HaveKey(id)); BOOST_CHECK_EQUAL(man->mapKeyMetadata.at(id).nCreateTime,42);
  BOOST_CHECK_EQUAL(man->GetTimeFirstKey(),birthday);
 }
 CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
 WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
 auto* man=wallet.GetLegacyScriptPubKeyMan(); BOOST_REQUIRE(man); BOOST_CHECK(man->HaveKey(id));
 BOOST_CHECK_EQUAL(man->mapKeyMetadata.at(id).nCreateTime,42);
}
BOOST_AUTO_TEST_CASE(begin_write_erase_failure_retry) {
 const std::vector<std::string> failures{"begin",DBKeys::KEYMETA,DBKeys::KEY,DBKeys::WATCHS,DBKeys::WATCHMETA};
 for(const auto& failure:failures) {
  const auto path=m_args.GetDataDirNet()/fs::PathFromString("fault-"+failure);
  CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey(); const auto id=pub.GetID();
  const auto script=GetScriptForDestination(PKHash(pub));
  {
   auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
   CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
   wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
   BOOST_REQUIRE(man->AddWatchOnly(script,42));
   f->fault.begin=failure=="begin"; f->fault.write=failure; f->fault.erase=failure;
   BOOST_CHECK(!man->ImportPrivKeys({{id,key}},99));
   BOOST_CHECK(!man->HaveKey(id)); BOOST_CHECK(man->HaveWatchOnly(script));
   f->fault=Fault{};
   BOOST_REQUIRE(man->ImportPrivKeys({{id,key}},99));
   BOOST_CHECK(man->HaveKey(id)); BOOST_CHECK(!man->HaveWatchOnly(script));
  }
  CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
  WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
  auto* man=wallet.GetLegacyScriptPubKeyMan();
  if (!man) { BOOST_ERROR("Successful retry lost key after reopen: " << failure); continue; }
  BOOST_CHECK(man->HaveKey(id)); BOOST_CHECK(!man->HaveWatchOnly(script));
 }
}
#ifndef WIN32
BOOST_AUTO_TEST_CASE(uncertain_completion_fails_closed) {
 for(int mode=0;mode<3;++mode) {
  const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("completion-%d",mode));
  const pid_t pid=fork(); BOOST_REQUIRE(pid>=0);
  if(pid==0) {
   std::set_terminate([]{_exit(86);});
   auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
   CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
   wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
   CKey key; key.MakeNewKey(true);
   if(mode<2) f->fault.commit_failure=mode+1;
   else { f->fault.write=DBKeys::KEY; f->fault.abort_failure=true; }
   man->ImportPrivKeys({{key.GetPubKey().GetID(),key}},42);
   _exit(0);
  }
  int status; BOOST_REQUIRE(waitpid(pid,&status,0)==pid);
  BOOST_CHECK_MESSAGE(WIFEXITED(status)&&WEXITSTATUS(status)==86,"mode="<<mode<<" status="<<status);
 }
}
#endif
#if defined(USE_SQLITE) && !defined(WIN32)
// Child isolation prevents a lifecycle violation terminating the whole test run.
BOOST_AUTO_TEST_CASE(real_sqlite_supplied_owner) {
 for(int mode=0;mode<3;++mode) {
  const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("sqlite-%d",mode));
  const pid_t pid=fork(); BOOST_REQUIRE(pid>=0);
  if(pid==0) {
   std::set_terminate([]{_exit(86);}); DatabaseOptions options; DatabaseStatus status; bilingual_str error;
   auto db=MakeSQLiteDatabase(path,options,status,error); if(!db)_exit(91); auto* sqlite=db.get();
   CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey();
   int result=0;
   {
    CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
    wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
    const auto script=GetScriptForRawPubKey(pub); if(!man->AddWatchOnly(script,42))_exit(90);
    {
    WalletBatch batch(wallet.GetDatabase()); if(!batch.TxnBegin())_exit(92);
    if(!man->AddKeyPubKeyWithDB(batch,key,pub))_exit(93);
    if(sqlite3_get_autocommit(sqlite->m_db)!=0)result=94;
    if(man->HaveKey(pub.GetID()))result=95;
    if(!man->HaveWatchOnly(script))result=89;
    if(mode==0 && !batch.TxnAbort())_exit(96);
    if(mode==2 && !batch.TxnCommit())_exit(97);
    }
    if(man->HaveKey(pub.GetID())!=(mode==2))result=98;
    if(man->HaveWatchOnly(script)!=(mode!=2))result=88;
   }
   auto reopened=MakeSQLiteDatabase(path,options,status,error); if(!reopened)_exit(87);
   CWallet wallet(nullptr,nullptr,"reopened",m_args,std::move(reopened)); LOCK(wallet.cs_wallet);
   WalletBatch batch(wallet.GetDatabase()); if(batch.LoadWallet(&wallet)!=DBErrors::LOAD_OK)_exit(85);
   auto* man=wallet.GetLegacyScriptPubKeyMan(); if(!man)_exit(84);
   if(man->HaveKey(pub.GetID())!=(mode==2))result=83;
   if(man->HaveWatchOnly(GetScriptForRawPubKey(pub))!=(mode!=2))result=82;
   _exit(result);
  }
  int status; BOOST_REQUIRE(waitpid(pid,&status,0)==pid);
  BOOST_CHECK_MESSAGE(WIFEXITED(status)&&WEXITSTATUS(status)==0,"mode="<<mode<<" status="<<status);
 }
}
#endif
#if defined(USE_SQLITE) && !defined(WIN32)
BOOST_AUTO_TEST_CASE(sqlite_generated_key_preserves_owner) {
 for(int mode=0;mode<3;++mode) {
  const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("sqlite-generated-%d",mode));
  const pid_t pid=fork(); BOOST_REQUIRE(pid>=0);
  if(pid==0) {
   std::set_terminate([]{_exit(86);}); DatabaseOptions options; DatabaseStatus status; bilingual_str error;
   auto db=MakeSQLiteDatabase(path,options,status,error); if(!db)_exit(91); auto* sqlite=db.get();
   CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
   wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
   CPubKey pub;
   {
    WalletBatch batch(wallet.GetDatabase()); if(!batch.TxnBegin())_exit(92);
    pub=man->GenerateNewKey(batch,0,false);
    if(sqlite3_get_autocommit(sqlite->m_db)!=0)_exit(93);
    if(man->HaveKey(pub.GetID()) || man->mapKeyMetadata.count(pub.GetID()))_exit(94);
    if(mode==0 && !batch.TxnAbort())_exit(95);
    if(mode==2 && !batch.TxnCommit())_exit(96);
   }
   if(man->HaveKey(pub.GetID())!=(mode==2))_exit(97);
   _exit(0);
  }
  int status; BOOST_REQUIRE(waitpid(pid,&status,0)==pid);
  BOOST_CHECK_MESSAGE(WIFEXITED(status)&&WEXITSTATUS(status)==0,"mode="<<mode<<" status="<<status);
 }
}
#endif
BOOST_AUTO_TEST_CASE(generated_key_metadata_waits_for_commit) {
 for(int mode=0;mode<3;++mode) {
  auto db=std::make_unique<FaultDB>(Open(m_args.GetDataDirNet()/fs::PathFromString(strprintf("generate-%d",mode)))); auto* f=db.get();
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
  wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
  const auto count=man->mapKeyMetadata.size(); const auto birthday=man->GetTimeFirstKey();
  WalletBatch batch(wallet.GetDatabase());
  if(mode==0) {
   f->fault.write=DBKeys::KEY;
   BOOST_CHECK_THROW(man->GenerateNewKey(batch,0,false),std::runtime_error);
   BOOST_CHECK_EQUAL(man->mapKeyMetadata.size(),count); BOOST_CHECK_EQUAL(man->GetTimeFirstKey(),birthday);
  } else {
   BOOST_REQUIRE(batch.TxnBegin()); const auto pub=man->GenerateNewKey(batch,0,false);
   BOOST_CHECK(!man->HaveKey(pub.GetID())); BOOST_CHECK_EQUAL(man->mapKeyMetadata.size(),count);
   if(mode==1) BOOST_REQUIRE(batch.TxnAbort()); else BOOST_REQUIRE(batch.TxnCommit());
   BOOST_CHECK_EQUAL(man->HaveKey(pub.GetID()),mode==2);
   BOOST_CHECK_EQUAL(man->mapKeyMetadata.count(pub.GetID()),mode==2 ? 1U : 0U);
  }
 }
}
BOOST_AUTO_TEST_CASE(encrypted_import_failure_retry) {
 const auto path=m_args.GetDataDirNet()/"encrypted-import";
 CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey();
 {
  auto db=std::make_unique<FaultDB>(Open(path)); auto* f=db.get();
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
  wallet.SetupLegacyScriptPubKeyMan();
  BOOST_REQUIRE(wallet.EncryptWallet(SecureString("synthetic")));
  BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic")));
  auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
  const auto script=GetScriptForRawPubKey(pub); BOOST_REQUIRE(man->AddWatchOnly(script,42));
  int notifications=0; auto connection=man->NotifyWatchonlyChanged.connect([&](bool){++notifications;});
  f->fault.write=DBKeys::CRYPTED_KEY;
  BOOST_CHECK(!man->ImportPrivKeys({{pub.GetID(),key}},99));
  BOOST_CHECK(!man->HaveKey(pub.GetID())); BOOST_CHECK(man->HaveWatchOnly(script));
  BOOST_CHECK_EQUAL(notifications,0); BOOST_CHECK_EQUAL(man->mapKeyMetadata.count(pub.GetID()),0U);
  f->fault=Fault{}; BOOST_REQUIRE(man->ImportPrivKeys({{pub.GetID(),key}},99));
  BOOST_CHECK(man->HaveKey(pub.GetID())); BOOST_CHECK(!man->HaveWatchOnly(script)); BOOST_CHECK_EQUAL(notifications,1);
  connection.disconnect();
 }
 CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
 WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
 BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic"))); auto* man=wallet.GetLegacyScriptPubKeyMan(); BOOST_REQUIRE(man);
 CKey loaded; BOOST_REQUIRE(man->GetKey(pub.GetID(),loaded)); BOOST_CHECK(loaded==key);
 BOOST_CHECK_EQUAL(man->mapKeyMetadata.at(pub.GetID()).nCreateTime,99);
 BOOST_CHECK(!wallet.GetDatabase().MakeBatch()->Exists(std::make_pair(DBKeys::KEY,pub)));
}
BOOST_AUTO_TEST_CASE(encrypted_owner_controls_publication) {
 for(int mode=0;mode<3;++mode) {
  const auto path=m_args.GetDataDirNet()/fs::PathFromString(strprintf("encrypted-owner-%d",mode));
  CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey(); const auto script=GetScriptForRawPubKey(pub);
  {
   CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(path)); LOCK(wallet.cs_wallet);
   wallet.SetupLegacyScriptPubKeyMan(); BOOST_REQUIRE(wallet.EncryptWallet(SecureString("synthetic")));
   BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic")));
   auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
   BOOST_REQUIRE(man->AddWatchOnly(script,42)); int notifications=0;
   auto connection=man->NotifyWatchonlyChanged.connect([&](bool){++notifications;});
   {
    WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
    BOOST_REQUIRE(man->AddKeyPubKeyWithDB(batch,key,pub));
    BOOST_CHECK(!man->HaveKey(pub.GetID())); BOOST_CHECK(man->HaveWatchOnly(script));
    BOOST_CHECK_EQUAL(man->mapKeyMetadata.count(pub.GetID()),0U); BOOST_CHECK_EQUAL(notifications,0);
    if(mode==0) BOOST_REQUIRE(batch.TxnAbort());
    if(mode==2) BOOST_REQUIRE(batch.TxnCommit());
   }
   BOOST_CHECK_EQUAL(man->HaveKey(pub.GetID()),mode==2); BOOST_CHECK_EQUAL(man->HaveWatchOnly(script),mode!=2);
   BOOST_CHECK_EQUAL(notifications,mode==2 ? 1 : 0); connection.disconnect();
  }
  CWallet wallet(nullptr,nullptr,"reopened",m_args,Open(path)); LOCK(wallet.cs_wallet);
  WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.LoadWallet(&wallet)==DBErrors::LOAD_OK);
  BOOST_REQUIRE(wallet.Unlock(SecureString("synthetic"))); auto* man=wallet.GetLegacyScriptPubKeyMan(); BOOST_REQUIRE(man);
  BOOST_CHECK_EQUAL(man->HaveKey(pub.GetID()),mode==2); BOOST_CHECK_EQUAL(man->HaveWatchOnly(script),mode!=2);
  if(mode==2) { CKey loaded; BOOST_REQUIRE(man->GetKey(pub.GetID(),loaded)); BOOST_CHECK(loaded==key); }
 }
}
BOOST_AUTO_TEST_CASE(direct_crypted_key_write_failure) {
 CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::make_unique<FaultDB>(Open(m_args.GetDataDirNet()/"direct-crypted"))); LOCK(wallet.cs_wallet);
 wallet.SetupLegacyScriptPubKeyMan(); BOOST_REQUIRE(wallet.EncryptWallet(SecureString("synthetic")));
 auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
 auto& db=dynamic_cast<FaultDB&>(wallet.GetDatabase());
 CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey(); const std::vector<unsigned char> crypted(48,1);
 db.fault.write=DBKeys::CRYPTED_KEY;
 BOOST_CHECK(!man->AddCryptedKey(pub,crypted)); BOOST_CHECK(!man->HaveKey(pub.GetID()));
 BOOST_CHECK_EQUAL(man->mapKeyMetadata.count(pub.GetID()),0U);
 db.fault=Fault{}; BOOST_REQUIRE(man->AddCryptedKey(pub,crypted)); BOOST_CHECK(man->HaveKey(pub.GetID()));
 BOOST_CHECK(wallet.GetDatabase().MakeBatch()->Exists(std::make_pair(DBKeys::CRYPTED_KEY,pub)));
}
BOOST_AUTO_TEST_CASE(joined_poison_discards_all_publication_and_retry_merges) {
 for(int mode=0;mode<2;++mode) {
  auto db=std::make_unique<FaultDB>(Open(m_args.GetDataDirNet()/fs::PathFromString(strprintf("delta-%d",mode)))); auto* f=db.get();
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,std::move(db)); LOCK(wallet.cs_wallet);
  wallet.SetupLegacyScriptPubKeyMan(); auto* man=wallet.GetLegacyScriptPubKeyMan(); LOCK(man->cs_KeyStore);
  CKey first,second; first.MakeNewKey(true); second.MakeNewKey(true); const auto a=first.GetPubKey(),b=second.GetPubKey();
  const auto script=GetScriptForRawPubKey(a); BOOST_REQUIRE(man->AddWatchOnly(script,42));
  int notifications=0; auto connection=man->NotifyWatchonlyChanged.connect([&](bool){++notifications;});
  WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
  BOOST_REQUIRE(man->AddKeyPubKeyWithDB(batch,first,a));
  if(mode==0) f->fault.write=DBKeys::KEY;
  BOOST_CHECK_EQUAL(man->AddKeyPubKeyWithDB(batch,second,b),mode!=0);
  BOOST_CHECK(!man->HaveKey(a.GetID())); BOOST_CHECK(!man->HaveKey(b.GetID()));
  BOOST_CHECK(man->HaveWatchOnly(script)); BOOST_CHECK_EQUAL(notifications,0);
  BOOST_CHECK_EQUAL(batch.TxnCommit(),mode!=0);
  if(mode==0) {
   BOOST_CHECK(!man->HaveKey(a.GetID())); BOOST_CHECK(!man->HaveKey(b.GetID()));
   BOOST_CHECK(man->HaveWatchOnly(script)); BOOST_CHECK_EQUAL(notifications,0);
   f->fault=Fault{}; BOOST_REQUIRE(batch.TxnBegin());
   BOOST_REQUIRE(man->AddKeyPubKeyWithDB(batch,first,a)); BOOST_REQUIRE(man->AddKeyPubKeyWithDB(batch,second,b));
   BOOST_REQUIRE(batch.TxnCommit());
  }
  BOOST_CHECK(man->HaveKey(a.GetID())); BOOST_CHECK(man->HaveKey(b.GetID()));
  BOOST_CHECK(!man->HaveWatchOnly(script)); CPubKey watched; BOOST_CHECK(!man->GetWatchPubKey(a.GetID(),watched));
  BOOST_CHECK_EQUAL(man->m_script_metadata.count(CScriptID(script)),0U); BOOST_CHECK_EQUAL(notifications,1);
  connection.disconnect();
 }
}
BOOST_AUTO_TEST_CASE(manager_destroyed_before_owner_completion) {
 for(int mode=0;mode<3;++mode) {
  CWallet wallet(nullptr,nullptr,"synthetic",m_args,Open(m_args.GetDataDirNet()/fs::PathFromString(strprintf("lifetime-%d",mode)))); LOCK(wallet.cs_wallet);
  CKey key; key.MakeNewKey(true); const auto pub=key.GetPubKey();
  WalletBatch batch(wallet.GetDatabase()); BOOST_REQUIRE(batch.TxnBegin());
  {
   auto man=std::make_unique<LegacyScriptPubKeyMan>(wallet); LOCK(man->cs_KeyStore);
   BOOST_REQUIRE(man->AddKeyPubKeyWithDB(batch,key,pub)); BOOST_CHECK(!man->HaveKey(pub.GetID()));
  }
  if(mode==0) BOOST_REQUIRE(batch.TxnAbort());
  if(mode==2) { BOOST_REQUIRE(batch.TxnCommit()); BOOST_CHECK(wallet.GetDatabase().MakeBatch()->Exists(std::make_pair(DBKeys::KEY,pub))); }
 }
}
BOOST_AUTO_TEST_SUITE_END()
}
