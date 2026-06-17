// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>

#include <addrman.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <clientversion.h>
#include <fs.h>
#include <hash.h>
#include <logging/timer.h>
#include <netbase.h>
#include <netgroup.h>
#include <random.h>
#include <streams.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/settings.h>
#include <util/system.h>
#include <util/translation.h>

#include <cstdio>
#include <cstdint>
#include <string>

namespace {

class DbNotFoundError : public std::exception
{
    using std::exception::exception;
};

template <typename Stream, typename Data>
bool SerializeDB(Stream& stream, const Data& data)
{
    // Write and commit header, data
    try {
        HashedSourceWriter hashwriter{stream};
        hashwriter << Params().MessageStart() << data;
        stream << hashwriter.GetHash();
    } catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

template <typename Data>
bool SerializeFileDB(const std::string& prefix, const fs::path& path, const Data& data, int version)
{
    // Generate random temporary filename
    const uint16_t randv{GetRand<uint16_t>()};
    std::string tmpfn = strprintf("%s.%04x", prefix, randv);

    // open temp output file, and associate with CAutoFile
    fs::path pathTmp = gArgs.GetDataDirNet() / fs::u8path(tmpfn);
    FILE *file = fsbridge::fopen(pathTmp, "wb");
    CAutoFile fileout(file, SER_DISK, version);
    if (fileout.IsNull()) {
        fileout.fclose();
        remove(pathTmp);
        return error("%s: Failed to open file %s", __func__, fs::PathToString(pathTmp));
    }

    // Serialize
    if (!SerializeDB(fileout, data)) {
        fileout.fclose();
        remove(pathTmp);
        return false;
    }
    if (!FileCommit(fileout.Get())) {
        fileout.fclose();
        remove(pathTmp);
        return error("%s: Failed to flush file %s", __func__, fs::PathToString(pathTmp));
    }
    fileout.fclose();

    // replace existing file, if any, with new file
    if (!RenameOver(pathTmp, path)) {
        remove(pathTmp);
        return error("%s: Rename-into-place failed", __func__);
    }

    return true;
}

template <typename Stream, typename Data>
void DeserializeDB(Stream& stream, Data& data, bool fCheckSum = true)
{
    CHashVerifier<Stream> verifier(&stream);
    // de-serialize file header (network specific magic number) and ..
    unsigned char pchMsgTmp[4];
    verifier >> pchMsgTmp;
    // ... verify the network matches ours
    if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
        throw std::runtime_error{"Invalid network magic number"};
    }

    // de-serialize data
    verifier >> data;

    // verify checksum
    if (fCheckSum) {
        uint256 hashTmp;
        stream >> hashTmp;
        if (hashTmp != verifier.GetHash()) {
            throw std::runtime_error{"Checksum mismatch, data corrupted"};
        }
    }
}

template <typename Data>
void DeserializeFileDB(const fs::path& path, Data& data, int version)
{
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(path, "rb");
    CAutoFile filein(file, SER_DISK, version);
    if (filein.IsNull()) {
        throw DbNotFoundError{};
    }
    DeserializeDB(filein, data);
}

fs::path PeersRefreshMarkerPath()
{
    return gArgs.GetDataDirNet() / fs::u8path(strprintf("peers.dat.reset-%d", CLIENT_VERSION));
}

bool WritePeersRefreshMarker()
{
    const fs::path marker_path = PeersRefreshMarkerPath();
    FILE* file = fsbridge::fopen(marker_path, "wb");
    if (file == nullptr) {
        return error("%s: Failed to open file %s", __func__, fs::PathToString(marker_path));
    }

    const std::string marker = strprintf("Smartiecoin peers.dat refreshed by client version %d\n", CLIENT_VERSION);
    if (std::fwrite(marker.data(), 1, marker.size(), file) != marker.size()) {
        std::fclose(file);
        return error("%s: Failed to write file %s", __func__, fs::PathToString(marker_path));
    }
    if (!FileCommit(file)) {
        std::fclose(file);
        return error("%s: Failed to flush file %s", __func__, fs::PathToString(marker_path));
    }
    std::fclose(file);
    return true;
}

bool ShouldRefreshPeersDat(const ArgsManager& args, const fs::path& path_addr)
{
    if (!fs::exists(path_addr)) {
        return false;
    }
    if (args.GetBoolArg("-resetpeers", false)) {
        return true;
    }
    return Params().NetworkIDString() == CBaseChainParams::MAIN && !fs::exists(PeersRefreshMarkerPath());
}

bool BackupPeersDat(const fs::path& path_addr, fs::path& backup_path)
{
    const std::string path = fs::PathToString(path_addr);
    for (int i = 0; i < 100; ++i) {
        const std::string suffix = i == 0 ? strprintf(".v%d.bak", CLIENT_VERSION) : strprintf(".v%d.bak.%d", CLIENT_VERSION, i);
        const fs::path candidate = fs::PathFromString(path + suffix);
        if (fs::exists(candidate)) {
            continue;
        }
        if (!RenameOver(path_addr, candidate)) {
            return false;
        }
        backup_path = candidate;
        return true;
    }
    return false;
}

bool RefreshPeersDatIfRequested(const ArgsManager& args, const NetGroupManager& netgroupman, int32_t check_addrman, std::unique_ptr<AddrMan>& addrman)
{
    const fs::path path_addr{gArgs.GetDataDirNet() / "peers.dat"};
    if (!ShouldRefreshPeersDat(args, path_addr)) {
        return false;
    }

    fs::path backup_path;
    if (!BackupPeersDat(path_addr, backup_path)) {
        LogPrintf("Unable to backup peers.dat for peer refresh. Continuing with existing peers.dat (%s)\n", fs::quoted(fs::PathToString(path_addr)));
        return false;
    }

    addrman = std::make_unique<AddrMan>(netgroupman, /*deterministic=*/false, /*consistency_check_ratio=*/check_addrman);
    LogPrintf("Refreshing peers.dat for a clean peer table. Original backed up to %s\n", fs::quoted(fs::PathToString(backup_path)));
    if (!DumpPeerAddresses(args, *addrman)) {
        LogPrintf("Warning: unable to write refreshed peers.dat; peers.dat may be recreated on next startup.\n");
        return true;
    }
    if (!WritePeersRefreshMarker()) {
        LogPrintf("Warning: unable to write peers refresh marker; peers.dat may be refreshed again on next startup.\n");
    }
    return true;
}
} // namespace

CBanDB::CBanDB(fs::path ban_list_path)
    : m_banlist_dat(ban_list_path + ".dat"),
      m_banlist_json(ban_list_path + ".json")
{
}

bool CBanDB::Write(const banmap_t& banSet)
{
    std::vector<std::string> errors;
    if (util::WriteSettings(m_banlist_json, {{JSON_KEY, BanMapToJson(banSet)}}, errors)) {
        return true;
    }

    for (const auto& err : errors) {
        error("%s", err);
    }
    return false;
}

bool CBanDB::Read(banmap_t& banSet)
{
    if (fs::exists(m_banlist_dat)) {
        LogPrintf("banlist.dat ignored because it can only be read by " PACKAGE_NAME " version 19.x. Remove %s to silence this warning.\n", fs::quoted(fs::PathToString(m_banlist_dat)));
    }
    // If the JSON banlist does not exist, then recreate it
    if (!fs::exists(m_banlist_json)) {
        return false;
    }

    std::map<std::string, util::SettingsValue> settings;
    std::vector<std::string> errors;

    if (!util::ReadSettings(m_banlist_json, settings, errors)) {
        for (const auto& err : errors) {
            LogPrintf("Cannot load banlist %s: %s\n", fs::PathToString(m_banlist_json), err);
        }
        return false;
    }

    try {
        BanMapFromJson(settings[JSON_KEY], banSet);
    } catch (const std::runtime_error& e) {
        LogPrintf("Cannot parse banlist %s: %s\n", fs::PathToString(m_banlist_json), e.what());
        return false;
    }

    return true;
}

bool DumpPeerAddresses(const ArgsManager& args, const AddrMan& addr)
{
    const auto pathAddr = gArgs.GetDataDirNet() / "peers.dat";
    return SerializeFileDB("peers", pathAddr, addr, CLIENT_VERSION);
}

void ReadFromStream(AddrMan& addr, CDataStream& ssPeers)
{
    DeserializeDB(ssPeers, addr, false);
}

std::optional<bilingual_str> LoadAddrman(const NetGroupManager& netgroupman, const ArgsManager& args, std::unique_ptr<AddrMan>& addrman)
{
    auto check_addrman = std::clamp<int32_t>(args.GetIntArg("-checkaddrman", DEFAULT_ADDRMAN_CONSISTENCY_CHECKS), 0, 1000000);
    addrman = std::make_unique<AddrMan>(netgroupman, /*deterministic=*/false, /*consistency_check_ratio=*/check_addrman);

    const auto start{SteadyClock::now()};
    const auto path_addr{gArgs.GetDataDirNet() / "peers.dat"};
    if (RefreshPeersDatIfRequested(args, netgroupman, check_addrman, addrman)) {
        return std::nullopt;
    }
    try {
        DeserializeFileDB(path_addr, *addrman, CLIENT_VERSION);
        LogPrintf("Loaded %i addresses from peers.dat  %dms\n", addrman->Size(), Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
    } catch (const DbNotFoundError&) {
        // Addrman can be in an inconsistent state after failure, reset it
        addrman = std::make_unique<AddrMan>(netgroupman, /*deterministic=*/false, /*consistency_check_ratio=*/check_addrman);
        LogPrintf("Creating peers.dat because the file was not found (%s)\n", fs::quoted(fs::PathToString(path_addr)));
        const bool peers_written = DumpPeerAddresses(args, *addrman);
        if (peers_written && Params().NetworkIDString() == CBaseChainParams::MAIN && !fs::exists(PeersRefreshMarkerPath()) && !WritePeersRefreshMarker()) {
            LogPrintf("Warning: unable to write peers refresh marker after creating peers.dat.\n");
        }
        if (!peers_written) {
            LogPrintf("Warning: unable to write peers.dat after creating a fresh peer table.\n");
        }
    } catch (const DbInconsistentError& e) {
        // Addrman has shown a tendency to corrupt itself even with graceful shutdowns on known-good
        // hardware. As the user would have to delete and recreate a new database regardless to cope
        // with frequent corruption, we are restoring old behaviour that does the same, silently.
        //
        // TODO: Evaluate cause and fix, revert this change at some point.
        addrman = std::make_unique<AddrMan>(netgroupman, /*deterministic=*/false, /*consistency_check_ratio=*/check_addrman);
        LogPrintf("Creating peers.dat because of invalid or corrupt file (%s)\n", e.what());
        DumpPeerAddresses(args, *addrman);
    } catch (const InvalidAddrManVersionError&) {
        if (!RenameOver(path_addr, (fs::path)path_addr + ".bak")) {
            addrman = nullptr;
            return strprintf(_("Failed to rename invalid peers.dat file. Please move or delete it and try again."));
        }
        // Addrman can be in an inconsistent state after failure, reset it
        addrman = std::make_unique<AddrMan>(netgroupman, /*deterministic=*/false, /*consistency_check_ratio=*/check_addrman);
        LogPrintf("Creating new peers.dat because the file version was not compatible (%s). Original backed up to peers.dat.bak\n", fs::quoted(fs::PathToString(path_addr)));
        DumpPeerAddresses(args, *addrman);
    } catch (const std::exception& e) {
        addrman = nullptr;
        return strprintf(_("Invalid or corrupt peers.dat (%s). If you believe this is a bug, please report it to %s. As a workaround, you can move the file (%s) out of the way (rename, move, or delete) to have a new one created on the next start."),
                         e.what(), PACKAGE_BUGREPORT, fs::quoted(fs::PathToString(path_addr)));
    }
    return std::nullopt;
}

void DumpAnchors(const fs::path& anchors_db_path, const std::vector<CAddress>& anchors)
{
    LOG_TIME_SECONDS(strprintf("Flush %d outbound block-relay-only peer addresses to anchors.dat", anchors.size()));
    SerializeFileDB("anchors", anchors_db_path, anchors, CLIENT_VERSION | ADDRV2_FORMAT);
}

std::vector<CAddress> ReadAnchors(const fs::path& anchors_db_path)
{
    std::vector<CAddress> anchors;
    try {
        DeserializeFileDB(anchors_db_path, anchors, CLIENT_VERSION | ADDRV2_FORMAT);
        LogPrintf("Loaded %i addresses from %s\n", anchors.size(), fs::quoted(fs::PathToString(anchors_db_path.filename())));
    } catch (const std::exception&) {
        anchors.clear();
    }

    fs::remove(anchors_db_path);
    return anchors;
}
