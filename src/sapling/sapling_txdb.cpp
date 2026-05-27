// Copyright (c) 2016-2020 The PIVX Core developers
// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <logging.h>
#include <sapling/incrementalmerkletree.h>

static constexpr uint8_t DB_SAPLING_ANCHOR{'Z'};
static constexpr uint8_t DB_SAPLING_NULLIFIER{'S'};
static constexpr uint8_t DB_BEST_SAPLING_ANCHOR{'z'};

bool CCoinsViewDB::GetSaplingAnchorAt(const uint256& rt, SaplingMerkleTree& tree) const
{
    if (rt == SaplingMerkleTree::empty_root()) {
        tree = SaplingMerkleTree{};
        return true;
    }
    return m_db->Read(std::make_pair(DB_SAPLING_ANCHOR, rt), tree);
}

bool CCoinsViewDB::GetNullifier(const uint256& nf) const
{
    bool spent = false;
    return m_db->Read(std::make_pair(DB_SAPLING_NULLIFIER, nf), spent) && spent;
}

uint256 CCoinsViewDB::GetBestAnchor() const
{
    uint256 hashBestAnchor;
    if (!m_db->Read(DB_BEST_SAPLING_ANCHOR, hashBestAnchor)) {
        return SaplingMerkleTree::empty_root();
    }
    return hashBestAnchor;
}

namespace {
void BatchWriteNullifiers(CDBBatch& batch, CNullifiersMap& mapToUse, uint8_t dbChar)
{
    size_t count = 0;
    size_t changed = 0;
    for (CNullifiersMap::iterator it = mapToUse.begin(); it != mapToUse.end();) {
        if (it->second.flags & CNullifiersCacheEntry::DIRTY) {
            if (!it->second.entered) {
                batch.Erase(std::make_pair(dbChar, it->first));
            } else {
                batch.Write(std::make_pair(dbChar, it->first), true);
            }
            ++changed;
        }
        ++count;
        it = mapToUse.erase(it);
    }
    LogPrint(BCLog::COINDB, "Committed %u changed Sapling nullifiers (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
}

template<typename Map, typename MapIterator, typename MapEntry, typename Tree>
void BatchWriteAnchors(CDBBatch& batch, Map& mapToUse, uint8_t dbChar)
{
    size_t count = 0;
    size_t changed = 0;
    for (MapIterator it = mapToUse.begin(); it != mapToUse.end();) {
        if (it->second.flags & MapEntry::DIRTY) {
            if (!it->second.entered) {
                batch.Erase(std::make_pair(dbChar, it->first));
            } else if (it->first != Tree::empty_root()) {
                batch.Write(std::make_pair(dbChar, it->first), it->second.tree);
            }
            ++changed;
        }
        ++count;
        it = mapToUse.erase(it);
    }
    LogPrint(BCLog::COINDB, "Committed %u changed Sapling anchors (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
}
} // namespace

bool CCoinsViewDB::BatchWriteSapling(const uint256& hashSaplingAnchor, CAnchorsSaplingMap& mapSaplingAnchors, CNullifiersMap& mapSaplingNullifiers, CDBBatch& batch)
{
    ::BatchWriteAnchors<CAnchorsSaplingMap, CAnchorsSaplingMap::iterator, CAnchorsSaplingCacheEntry, SaplingMerkleTree>(batch, mapSaplingAnchors, DB_SAPLING_ANCHOR);
    ::BatchWriteNullifiers(batch, mapSaplingNullifiers, DB_SAPLING_NULLIFIER);
    if (!hashSaplingAnchor.IsNull()) {
        batch.Write(DB_BEST_SAPLING_ANCHOR, hashSaplingAnchor);
    }
    return true;
}
