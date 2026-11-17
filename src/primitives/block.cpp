// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <hash_x11.h>
#include <streams.h>
#include <tinyformat.h>

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
/* Memoization of the memory-hard header PoW hash (yespower).
 *
 * GetHash() is expensive (milliseconds, 8 MiB scratchpad with pre-v0.5.0
 * parameters). Inbound peers can replay already-known valid headers/blocks
 * indefinitely without being punished, and the P2P paths hash each header
 * several times before deduplication. Without memoization this pegs the
 * message-handler thread and starves RPC (cs_main).
 *
 * The cache is keyed by the exact 80 serialized header bytes, so a hit always
 * returns the identical hash the uncached computation would produce. This is
 * a pure performance optimization: no consensus or validation semantics change.
 * Bounded LRU; ~1 MB at the default cap. */
struct HeaderHashCache {
    std::mutex mutex;
    std::list<std::string> lru; // front = most recently used
    std::unordered_map<std::string, std::pair<uint256, std::list<std::string>::iterator>> entries;
};
HeaderHashCache g_header_hash_cache;
} // namespace

uint256 CBlockHeader::GetHash() const
{
    std::vector<unsigned char> vch(80);
    CVectorWriter ss(SER_GETHASH, PROTOCOL_VERSION, vch, 0);
    ss << *this;
    const std::string key(reinterpret_cast<const char*>(vch.data()), vch.size());

    {
        std::lock_guard<std::mutex> lock(g_header_hash_cache.mutex);
        const auto it = g_header_hash_cache.entries.find(key);
        if (it != g_header_hash_cache.entries.end()) {
            g_header_hash_cache.lru.splice(g_header_hash_cache.lru.begin(), g_header_hash_cache.lru, it->second.second);
            return it->second.first;
        }
    }

    const uint256 hash = HashX11((const char*)vch.data(), (const char*)vch.data() + vch.size());

    {
        std::lock_guard<std::mutex> lock(g_header_hash_cache.mutex);
        const auto it = g_header_hash_cache.entries.find(key);
        if (it != g_header_hash_cache.entries.end()) {
            // Another thread computed the same bytes meanwhile; identical result.
            g_header_hash_cache.lru.splice(g_header_hash_cache.lru.begin(), g_header_hash_cache.lru, it->second.second);
            return it->second.first;
        }
        if (g_header_hash_cache.entries.size() >= HEADER_HASH_CACHE_MAX_ENTRIES) {
            g_header_hash_cache.entries.erase(g_header_hash_cache.lru.back());
            g_header_hash_cache.lru.pop_back();
        }
        g_header_hash_cache.lru.push_front(key);
        g_header_hash_cache.entries.emplace(key, std::make_pair(hash, g_header_hash_cache.lru.begin()));
    }
    return hash;
}

void ClearHeaderHashCache()
{
    std::lock_guard<std::mutex> lock(g_header_hash_cache.mutex);
    g_header_hash_cache.entries.clear();
    g_header_hash_cache.lru.clear();
}

size_t HeaderHashCacheSize()
{
    std::lock_guard<std::mutex> lock(g_header_hash_cache.mutex);
    return g_header_hash_cache.entries.size();
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}

static void MarkVersionAsMostRecent(std::list<int32_t>& last_unique_versions, std::list<int32_t>::const_iterator version_it)
{
    if (version_it != last_unique_versions.cbegin()) {
        // Move the found version to the front of the list
        last_unique_versions.splice(last_unique_versions.begin(), last_unique_versions, version_it, std::next(version_it));
    }
}

static void SaveVersionAsMostRecent(std::list<int32_t>& last_unique_versions, const int32_t version)
{
    last_unique_versions.push_front(version);

    // Always keep the last 7 unique versions
    constexpr std::size_t max_backwards_look_ups = 7;
    if (last_unique_versions.size() > max_backwards_look_ups) {
        // Evict the oldest version
        last_unique_versions.pop_back();
    }
}

void CompressibleBlockHeader::Compress(const std::vector<CompressibleBlockHeader>& previous_blocks, std::list<int32_t>& last_unique_versions)
{
    if (previous_blocks.empty()) {
        // Previous block not available, we have to send the block completely uncompressed
        SaveVersionAsMostRecent(last_unique_versions, nVersion);
        return;
    }

    // Try to compress version
    const auto version_it = std::find(last_unique_versions.cbegin(), last_unique_versions.cend(), nVersion);
    if (version_it != last_unique_versions.cend()) {
        // Version is found in the last 7 unique blocks.
        bit_field.SetVersionOffset(static_cast<uint8_t>(std::distance(last_unique_versions.cbegin(), version_it) + 1));

        // Mark the version as the most recent one
        MarkVersionAsMostRecent(last_unique_versions, version_it);
    } else {
        // Save the version as the most recent one
        SaveVersionAsMostRecent(last_unique_versions, nVersion);
    }

    // Previous block is available
    const auto& last_block = previous_blocks.back();
    bit_field.MarkAsCompressed(CompressedHeaderBitField::Flag::PREV_BLOCK_HASH);

    // Compute compressed time diff
    const int64_t time_diff = nTime - last_block.nTime;
    if (time_diff <= std::numeric_limits<int16_t>::max() && time_diff >= std::numeric_limits<int16_t>::min()) {
        time_offset = static_cast<int16_t>(time_diff);
        bit_field.MarkAsCompressed(CompressedHeaderBitField::Flag::TIMESTAMP);
    }

    // If n_bits matches previous block, it can be compressed (not sent at all)
    if (nBits == last_block.nBits) {
        bit_field.MarkAsCompressed(CompressedHeaderBitField::Flag::NBITS);
    }
}

void CompressibleBlockHeader::Uncompress(const std::vector<CBlockHeader>& previous_blocks, std::list<int32_t>& last_unique_versions)
{
    if (previous_blocks.empty()) {
        // First block in chain is always uncompressed
        SaveVersionAsMostRecent(last_unique_versions, nVersion);
        return;
    }

    // We have the previous block
    const auto& last_block = previous_blocks.back();

    // Uncompress version
    if (bit_field.IsVersionCompressed()) {
        const auto version_offset = bit_field.GetVersionOffset();
        if (version_offset <= last_unique_versions.size()) {
            auto version_it = last_unique_versions.begin();
            std::advance(version_it, version_offset - 1);
            nVersion = *version_it;
            MarkVersionAsMostRecent(last_unique_versions, version_it);
        }
    } else {
        // Save the version as the most recent one
        SaveVersionAsMostRecent(last_unique_versions, nVersion);
    }

    // Uncompress prev block hash
    if (bit_field.IsCompressed(CompressedHeaderBitField::Flag::PREV_BLOCK_HASH)) {
        hashPrevBlock = last_block.GetHash();
    }

    // Uncompress timestamp
    if (bit_field.IsCompressed(CompressedHeaderBitField::Flag::TIMESTAMP)) {
        nTime = last_block.nTime + time_offset;
    }

    // Uncompress n_bits
    if (bit_field.IsCompressed(CompressedHeaderBitField::Flag::NBITS)) {
        nBits = last_block.nBits;
    }
}
