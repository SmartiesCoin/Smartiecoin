// Copyright (c) 2012-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>

#include <consensus/consensus.h>
#include <logging.h>
#include <random.h>
#include <util/trace.h>
#include <version.h>

bool CCoinsView::GetCoin(const COutPoint &outpoint, Coin &coin) const { return false; }
uint256 CCoinsView::GetBestBlock() const { return uint256(); }
std::vector<uint256> CCoinsView::GetHeadBlocks() const { return std::vector<uint256>(); }
bool CCoinsView::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlock, bool erase, const uint256& hashSaplingAnchor, CAnchorsSaplingMap* mapSaplingAnchors, CNullifiersMap* mapSaplingNullifiers) { return false; }
std::unique_ptr<CCoinsViewCursor> CCoinsView::Cursor() const { return nullptr; }
bool CCoinsView::GetSaplingAnchorAt(const uint256& rt, SaplingMerkleTree& tree) const { return false; }
bool CCoinsView::GetNullifier(const uint256& nullifier) const { return false; }
uint256 CCoinsView::GetBestAnchor() const { return SaplingMerkleTree::empty_root(); }

bool CCoinsView::HaveCoin(const COutPoint &outpoint) const
{
    Coin coin;
    return GetCoin(outpoint, coin);
}

CCoinsViewBacked::CCoinsViewBacked(CCoinsView *viewIn) : base(viewIn) { }
bool CCoinsViewBacked::GetCoin(const COutPoint &outpoint, Coin &coin) const { return base->GetCoin(outpoint, coin); }
bool CCoinsViewBacked::HaveCoin(const COutPoint &outpoint) const { return base->HaveCoin(outpoint); }
uint256 CCoinsViewBacked::GetBestBlock() const { return base->GetBestBlock(); }
std::vector<uint256> CCoinsViewBacked::GetHeadBlocks() const { return base->GetHeadBlocks(); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
bool CCoinsViewBacked::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlock, bool erase, const uint256& hashSaplingAnchor, CAnchorsSaplingMap* mapSaplingAnchors, CNullifiersMap* mapSaplingNullifiers) { return base->BatchWrite(mapCoins, hashBlock, erase, hashSaplingAnchor, mapSaplingAnchors, mapSaplingNullifiers); }
std::unique_ptr<CCoinsViewCursor> CCoinsViewBacked::Cursor() const { return base->Cursor(); }
size_t CCoinsViewBacked::EstimateSize() const { return base->EstimateSize(); }
bool CCoinsViewBacked::GetSaplingAnchorAt(const uint256& rt, SaplingMerkleTree& tree) const { return base->GetSaplingAnchorAt(rt, tree); }
bool CCoinsViewBacked::GetNullifier(const uint256& nullifier) const { return base->GetNullifier(nullifier); }
uint256 CCoinsViewBacked::GetBestAnchor() const { return base->GetBestAnchor(); }

CCoinsViewCache::CCoinsViewCache(CCoinsView* baseIn, bool deterministic) :
    CCoinsViewBacked(baseIn), m_deterministic(deterministic),
    cacheCoins(0, SaltedOutpointHasher(/*deterministic=*/deterministic), CCoinsMap::key_equal{}, &m_cache_coins_memory_resource)
{}

size_t CCoinsViewCache::DynamicMemoryUsage() const {
    return memusage::DynamicUsage(cacheCoins) +
           memusage::DynamicUsage(cacheSaplingAnchors) +
           memusage::DynamicUsage(cacheSaplingNullifiers) +
           cachedCoinsUsage;
}

CCoinsMap::iterator CCoinsViewCache::FetchCoin(const COutPoint &outpoint) const {
    CCoinsMap::iterator it = cacheCoins.find(outpoint);
    if (it != cacheCoins.end())
        return it;
    Coin tmp;
    if (!base->GetCoin(outpoint, tmp))
        return cacheCoins.end();
    CCoinsMap::iterator ret = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::forward_as_tuple(std::move(tmp))).first;
    if (ret->second.coin.IsSpent()) {
        // The parent only has an empty entry for this outpoint; we can consider our
        // version as fresh.
        ret->second.flags = CCoinsCacheEntry::FRESH;
    }
    cachedCoinsUsage += ret->second.coin.DynamicMemoryUsage();
    return ret;
}

bool CCoinsViewCache::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it != cacheCoins.end()) {
        coin = it->second.coin;
        return !coin.IsSpent();
    }
    return false;
}

void CCoinsViewCache::AddCoin(const COutPoint &outpoint, Coin&& coin, bool possible_overwrite) {
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::tuple<>());
    bool fresh = false;
    if (!inserted) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    }
    if (!possible_overwrite) {
        if (!it->second.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // If the coin exists in this cache as a spent coin and is DIRTY, then
        // its spentness hasn't been flushed to the parent cache. We're
        // re-adding the coin to this cache now but we can't mark it as FRESH.
        // If we mark it FRESH and then spend it before the cache is flushed
        // we would remove it from this cache and would never flush spentness
        // to the parent cache.
        //
        // Re-adding a spent coin can happen in the case of a re-org (the coin
        // is 'spent' when the block adding it is disconnected and then
        // re-added when it is also added in a newly connected block).
        //
        // If the coin doesn't exist in the current cache, or is spent but not
        // DIRTY, then it can be marked FRESH.
        fresh = !(it->second.flags & CCoinsCacheEntry::DIRTY);
    }
    it->second.coin = std::move(coin);
    it->second.flags |= CCoinsCacheEntry::DIRTY | (fresh ? CCoinsCacheEntry::FRESH : 0);
    cachedCoinsUsage += it->second.coin.DynamicMemoryUsage();
    TRACE5(utxocache, add,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
}

void CCoinsViewCache::EmplaceCoinInternalDANGER(COutPoint&& outpoint, Coin&& coin) {
    cachedCoinsUsage += coin.DynamicMemoryUsage();
    cacheCoins.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(std::move(outpoint)),
        std::forward_as_tuple(std::move(coin), CCoinsCacheEntry::DIRTY));
}

void AddCoins(CCoinsViewCache& cache, const CTransaction &tx, int nHeight, bool check_for_overwrite) {
    bool fCoinbase = tx.IsCoinBase();
    const uint256& txid = tx.GetHash();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        bool overwrite = check_for_overwrite ? cache.HaveCoin(COutPoint(txid, i)) : fCoinbase;
        // Coinbase transactions can always be overwritten, in order to correctly
        // deal with the pre-BIP30 occurrences of duplicate coinbase transactions.
        cache.AddCoin(COutPoint(txid, i), Coin(tx.vout[i], nHeight, fCoinbase), overwrite);
    }
}

bool CCoinsViewCache::SpendCoin(const COutPoint &outpoint, Coin* moveout) {
    CCoinsMap::iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) return false;
    cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    TRACE5(utxocache, spent,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
    if (moveout) {
        *moveout = std::move(it->second.coin);
    }
    if (it->second.flags & CCoinsCacheEntry::FRESH) {
        cacheCoins.erase(it);
    } else {
        it->second.flags |= CCoinsCacheEntry::DIRTY;
        it->second.coin.Clear();
    }
    return true;
}

static const Coin coinEmpty;

const Coin& CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) {
        return coinEmpty;
    } else {
        return it->second.coin;
    }
}

bool CCoinsViewCache::HaveCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

bool CCoinsViewCache::HaveCoinInCache(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = cacheCoins.find(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

uint256 CCoinsViewCache::GetBestBlock() const {
    if (hashBlock.IsNull())
        hashBlock = base->GetBestBlock();
    return hashBlock;
}

void CCoinsViewCache::SetBestBlock(const uint256 &hashBlockIn) {
    hashBlock = hashBlockIn;
}

namespace {
template<typename Map, typename MapIterator, typename MapEntry>
void BatchWriteAnchors(Map& mapAnchors, Map& cacheAnchors, size_t& cachedCoinsUsage, bool erase)
{
    for (MapIterator child_it = mapAnchors.begin(); child_it != mapAnchors.end();) {
        if (child_it->second.flags & MapEntry::DIRTY) {
            MapIterator parent_it = cacheAnchors.find(child_it->first);
            if (parent_it == cacheAnchors.end()) {
                MapEntry& entry = cacheAnchors[child_it->first];
                entry.entered = child_it->second.entered;
                entry.tree = child_it->second.tree;
                entry.flags = MapEntry::DIRTY;
                cachedCoinsUsage += entry.tree.DynamicMemoryUsage();
            } else {
                MapEntry& entry = parent_it->second;
                const bool update_tree = child_it->second.entered && !(entry.tree == child_it->second.tree);
                if (entry.entered != child_it->second.entered || update_tree) {
                    if (update_tree) {
                        cachedCoinsUsage -= entry.tree.DynamicMemoryUsage();
                        entry.tree = child_it->second.tree;
                        cachedCoinsUsage += entry.tree.DynamicMemoryUsage();
                    }
                    entry.entered = child_it->second.entered;
                    entry.flags |= MapEntry::DIRTY;
                }
            }
        }
        if (erase) {
            child_it = mapAnchors.erase(child_it);
        } else {
            child_it->second.flags = 0;
            ++child_it;
        }
    }
}

void BatchWriteNullifiers(CNullifiersMap& mapNullifiers, CNullifiersMap& cacheNullifiers, bool erase)
{
    for (CNullifiersMap::iterator child_it = mapNullifiers.begin(); child_it != mapNullifiers.end();) {
        if (child_it->second.flags & CNullifiersCacheEntry::DIRTY) {
            CNullifiersMap::iterator parent_it = cacheNullifiers.find(child_it->first);
            if (parent_it == cacheNullifiers.end()) {
                CNullifiersCacheEntry& entry = cacheNullifiers[child_it->first];
                entry.entered = child_it->second.entered;
                entry.flags = CNullifiersCacheEntry::DIRTY;
            } else if (parent_it->second.entered != child_it->second.entered) {
                parent_it->second.entered = child_it->second.entered;
                parent_it->second.flags |= CNullifiersCacheEntry::DIRTY;
            }
        }
        if (erase) {
            child_it = mapNullifiers.erase(child_it);
        } else {
            child_it->second.flags = 0;
            ++child_it;
        }
    }
}
} // namespace

bool CCoinsViewCache::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlockIn, bool erase, const uint256& hashSaplingAnchorIn, CAnchorsSaplingMap* mapSaplingAnchors, CNullifiersMap* mapSaplingNullifiers) {
    for (CCoinsMap::iterator it = mapCoins.begin();
            it != mapCoins.end();
            it = erase ? mapCoins.erase(it) : std::next(it)) {
        // Ignore non-dirty entries (optimization).
        if (!(it->second.flags & CCoinsCacheEntry::DIRTY)) {
            continue;
        }
        CCoinsMap::iterator itUs = cacheCoins.find(it->first);
        if (itUs == cacheCoins.end()) {
            // The parent cache does not have an entry, while the child cache does.
            // We can ignore it if it's both spent and FRESH in the child
            if (!(it->second.flags & CCoinsCacheEntry::FRESH && it->second.coin.IsSpent())) {
                // Create the coin in the parent cache, move the data up
                // and mark it as dirty.
                CCoinsCacheEntry& entry = cacheCoins[it->first];
                if (erase) {
                    // The `move` call here is purely an optimization; we rely on the
                    // `mapCoins.erase` call in the `for` expression to actually remove
                    // the entry from the child map.
                    entry.coin = std::move(it->second.coin);
                } else {
                    entry.coin = it->second.coin;
                }
                cachedCoinsUsage += entry.coin.DynamicMemoryUsage();
                entry.flags = CCoinsCacheEntry::DIRTY;
                // We can mark it FRESH in the parent if it was FRESH in the child
                // Otherwise it might have just been flushed from the parent's cache
                // and already exist in the grandparent
                if (it->second.flags & CCoinsCacheEntry::FRESH) {
                    entry.flags |= CCoinsCacheEntry::FRESH;
                }
            }
        } else {
            // Found the entry in the parent cache
            if ((it->second.flags & CCoinsCacheEntry::FRESH) && !itUs->second.coin.IsSpent()) {
                // The coin was marked FRESH in the child cache, but the coin
                // exists in the parent cache. If this ever happens, it means
                // the FRESH flag was misapplied and there is a logic error in
                // the calling code.
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }

            if ((itUs->second.flags & CCoinsCacheEntry::FRESH) && it->second.coin.IsSpent()) {
                // The grandparent cache does not have an entry, and the coin
                // has been spent. We can just delete it from the parent cache.
                cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                cacheCoins.erase(itUs);
            } else {
                // A normal modification.
                cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                if (erase) {
                    // The `move` call here is purely an optimization; we rely on the
                    // `mapCoins.erase` call in the `for` expression to actually remove
                    // the entry from the child map.
                    itUs->second.coin = std::move(it->second.coin);
                } else {
                    itUs->second.coin = it->second.coin;
                }
                cachedCoinsUsage += itUs->second.coin.DynamicMemoryUsage();
                itUs->second.flags |= CCoinsCacheEntry::DIRTY;
                // NOTE: It isn't safe to mark the coin as FRESH in the parent
                // cache. If it already existed and was spent in the parent
                // cache then marking it FRESH would prevent that spentness
                // from being flushed to the grandparent.
            }
        }
    }
    if (mapSaplingAnchors != nullptr) {
        ::BatchWriteAnchors<CAnchorsSaplingMap, CAnchorsSaplingMap::iterator, CAnchorsSaplingCacheEntry>(*mapSaplingAnchors, cacheSaplingAnchors, cachedCoinsUsage, erase);
    }
    if (mapSaplingNullifiers != nullptr) {
        ::BatchWriteNullifiers(*mapSaplingNullifiers, cacheSaplingNullifiers, erase);
    }
    if (mapSaplingAnchors != nullptr || mapSaplingNullifiers != nullptr) {
        hashSaplingAnchor = hashSaplingAnchorIn;
    }
    hashBlock = hashBlockIn;
    return true;
}

bool CCoinsViewCache::Flush() {
    bool fOk = base->BatchWrite(cacheCoins, hashBlock, /*erase=*/true, hashSaplingAnchor, &cacheSaplingAnchors, &cacheSaplingNullifiers);
    if (fOk) {
        if (!cacheCoins.empty()) {
            /* BatchWrite must erase all cacheCoins elements when erase=true. */
            throw std::logic_error("Not all cached coins were erased");
        }
        if (!cacheSaplingAnchors.empty() || !cacheSaplingNullifiers.empty()) {
            throw std::logic_error("Not all cached Sapling state was erased");
        }
        ReallocateCache();
    }
    cachedCoinsUsage = 0;
    return fOk;
}

bool CCoinsViewCache::Sync()
{
    bool fOk = base->BatchWrite(cacheCoins, hashBlock, /*erase=*/false, hashSaplingAnchor, &cacheSaplingAnchors, &cacheSaplingNullifiers);
    // Instead of clearing `cacheCoins` as we would in Flush(), just clear the
    // FRESH/DIRTY flags of any coin that isn't spent.
    for (auto it = cacheCoins.begin(); it != cacheCoins.end(); ) {
        if (it->second.coin.IsSpent()) {
            cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
            it = cacheCoins.erase(it);
        } else {
            it->second.flags = 0;
            ++it;
        }
    }
    return fOk;
}

void CCoinsViewCache::Uncache(const COutPoint& hash)
{
    CCoinsMap::iterator it = cacheCoins.find(hash);
    if (it != cacheCoins.end() && it->second.flags == 0) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
        TRACE5(utxocache, uncache,
               hash.hash.data(),
               (uint32_t)hash.n,
               (uint32_t)it->second.coin.nHeight,
               (int64_t)it->second.coin.out.nValue,
               (bool)it->second.coin.IsCoinBase());
        cacheCoins.erase(it);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const {
    return cacheCoins.size();
}

bool CCoinsViewCache::GetSaplingAnchorAt(const uint256& rt, SaplingMerkleTree& tree) const
{
    CAnchorsSaplingMap::const_iterator it = cacheSaplingAnchors.find(rt);
    if (it != cacheSaplingAnchors.end()) {
        if (it->second.entered) {
            tree = it->second.tree;
            return true;
        }
        return false;
    }

    if (!base->GetSaplingAnchorAt(rt, tree)) {
        return false;
    }

    CAnchorsSaplingMap::iterator ret = cacheSaplingAnchors.emplace(rt, CAnchorsSaplingCacheEntry()).first;
    ret->second.entered = true;
    ret->second.tree = tree;
    cachedCoinsUsage += ret->second.tree.DynamicMemoryUsage();
    return true;
}

bool CCoinsViewCache::GetNullifier(const uint256& nullifier) const
{
    CNullifiersMap::const_iterator it = cacheSaplingNullifiers.find(nullifier);
    if (it != cacheSaplingNullifiers.end()) {
        return it->second.entered;
    }

    CNullifiersCacheEntry entry;
    entry.entered = base->GetNullifier(nullifier);
    cacheSaplingNullifiers.emplace(nullifier, entry);
    return entry.entered;
}

uint256 CCoinsViewCache::GetBestAnchor() const
{
    if (hashSaplingAnchor.IsNull()) {
        hashSaplingAnchor = base->GetBestAnchor();
    }
    return hashSaplingAnchor;
}

void CCoinsViewCache::PushAnchor(const SaplingMerkleTree& tree)
{
    const uint256 newRoot = tree.root();
    const uint256 currentRoot = GetBestAnchor();
    if (currentRoot == newRoot) {
        return;
    }

    auto insertRet = cacheSaplingAnchors.emplace(newRoot, CAnchorsSaplingCacheEntry());
    CAnchorsSaplingCacheEntry& entry = insertRet.first->second;
    if (!insertRet.second) {
        cachedCoinsUsage -= entry.tree.DynamicMemoryUsage();
    }
    entry.entered = true;
    entry.tree = tree;
    entry.flags |= CAnchorsSaplingCacheEntry::DIRTY;
    cachedCoinsUsage += entry.tree.DynamicMemoryUsage();
    hashSaplingAnchor = newRoot;
}

void CCoinsViewCache::PopAnchor(const uint256& newrt)
{
    const uint256 currentRoot = GetBestAnchor();
    if (currentRoot == newrt) {
        return;
    }

    SaplingMerkleTree tree;
    assert(GetSaplingAnchorAt(currentRoot, tree));
    CAnchorsSaplingCacheEntry& entry = cacheSaplingAnchors[currentRoot];
    entry.entered = false;
    entry.flags |= CAnchorsSaplingCacheEntry::DIRTY;
    hashSaplingAnchor = newrt;
}

void CCoinsViewCache::SetNullifiers(const CTransaction& tx, bool spent)
{
    if (!tx.HasShieldedPayloadField()) {
        return;
    }
    for (const SpendDescription& spendDescription : tx.sapData.vShieldedSpend) {
        CNullifiersCacheEntry& entry = cacheSaplingNullifiers[spendDescription.nullifier];
        entry.entered = spent;
        entry.flags |= CNullifiersCacheEntry::DIRTY;
    }
}

bool CCoinsViewCache::HaveShieldedRequirements(const CTransaction& tx) const
{
    if (!tx.HasShieldedPayload()) {
        return true;
    }
    for (const SpendDescription& spendDescription : tx.sapData.vShieldedSpend) {
        if (GetNullifier(spendDescription.nullifier)) {
            return false;
        }
        SaplingMerkleTree tree;
        if (!GetSaplingAnchorAt(spendDescription.anchor, tree)) {
            return false;
        }
    }
    return true;
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            if (!HaveCoin(tx.vin[i].prevout)) {
                return false;
            }
        }
    }
    return true;
}

void CCoinsViewCache::ReallocateCache()
{
    // Cache should be empty when we're calling this.
    assert(cacheCoins.size() == 0);
    cacheCoins.~CCoinsMap();
    m_cache_coins_memory_resource.~CCoinsMapMemoryResource();
    ::new (&m_cache_coins_memory_resource) CCoinsMapMemoryResource{};
    ::new (&cacheCoins) CCoinsMap{0, SaltedOutpointHasher{/*deterministic=*/m_deterministic}, CCoinsMap::key_equal{}, &m_cache_coins_memory_resource};
}

void CCoinsViewCache::SanityCheck() const
{
    size_t recomputed_usage = 0;
    for (const auto& [_, entry] : cacheCoins) {
        unsigned attr = 0;
        if (entry.flags & CCoinsCacheEntry::DIRTY) attr |= 1;
        if (entry.flags & CCoinsCacheEntry::FRESH) attr |= 2;
        if (entry.coin.IsSpent()) attr |= 4;
        // Only 5 combinations are possible.
        assert(attr != 2 && attr != 4 && attr != 7);

        // Recompute cachedCoinsUsage.
        recomputed_usage += entry.coin.DynamicMemoryUsage();
    }
    for (const auto& [_, entry] : cacheSaplingAnchors) {
        recomputed_usage += entry.tree.DynamicMemoryUsage();
    }
    assert(recomputed_usage == cachedCoinsUsage);
}

static const size_t MAX_OUTPUTS_PER_BLOCK = MaxBlockSize() /  ::GetSerializeSize(CTxOut(), PROTOCOL_VERSION);

const Coin& AccessByTxid(const CCoinsViewCache& view, const uint256& txid)
{
    COutPoint iter(txid, 0);
    while (iter.n < MAX_OUTPUTS_PER_BLOCK) {
        const Coin& alternate = view.AccessCoin(iter);
        if (!alternate.IsSpent()) return alternate;
        ++iter.n;
    }
    return coinEmpty;
}

bool CCoinsViewErrorCatcher::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    try {
        return CCoinsViewBacked::GetCoin(outpoint, coin);
    } catch(const std::runtime_error& e) {
        for (const auto& f : m_err_callbacks) {
            f();
        }
        LogPrintf("Error reading from database: %s\n", e.what());
        // Starting the shutdown sequence and returning false to the caller would be
        // interpreted as 'entry not found' (as opposed to unable to read data), and
        // could lead to invalid interpretation. Just exit immediately, as we can't
        // continue anyway, and all writes should be atomic.
        std::abort();
    }
}
