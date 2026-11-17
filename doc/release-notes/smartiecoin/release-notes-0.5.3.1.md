Smartiecoin Core version 0.5.3.1
================================

Release is now available from:

  <https://github.com/SmartiesCoin/Smartiecoin/releases>

This is a hotfix release on top of 0.5.3. It fixes a remote CPU-exhaustion
bug affecting listening nodes. Consensus rules are NOT changed.

Please report bugs using the issue tracker at github:

  <https://github.com/SmartiesCoin/Smartiecoin/issues>

0.5.3.1 change log
-------------------

### Network / denial-of-service fix

- `CBlockHeader::GetHash()` runs the memory-hard yespower PoW hash (8 MiB
  scratchpad before the v0.5.0 fork, ~3.3 ms per hash measured on Apple
  Silicon). Replayed `headers` and `block` messages forced repeated full
  PoW hashes of already-known headers while holding `cs_main`, letting a
  single peer peg the `d-msghand` thread at ~100% of one core and stall
  RPC on listening nodes (`listen=1`).
- `GetHash()` now memoizes results in a bounded, thread-safe LRU cache
  (4096 entries, ~1 MB) keyed by the exact 80 serialized header bytes. A
  cache hit returns the bit-identical hash with no PoW work; any mutated
  header byte is a different key and is hashed fresh. Validation order and
  consensus rules are unchanged.
- The `received block` net log line no longer computes the block hash a
  second time (log arguments are evaluated even when the category is
  disabled).

### Tests

- New unit suite `pow_tests`: cache hits are bit-identical to raw yespower,
  every single-byte header mutation misses, the LRU eviction bound holds,
  and concurrent readers/writers stay consistent.
- New functional test `p2p_pow_hash_replay.py`: mines a 2000-block regtest
  chain, replays the full header chain and the blocks from a peer, and
  requires the node to stay responsive (ping/pong round-trips under 20 s
  during and after the flood). The test fails against pre-fix binaries.

### Versioning

- Adds an optional fourth version component (hotfix) so patch releases can
  be numbered `0.5.3.1` without changing the P2P protocol version integer.
  Windows installer (`setup.nsi`) and macOS bundle (`Info.plist`) metadata
  follow the new version.

Credits
-------

Thanks to Gatevia for reporting the `d-msghand` CPU exhaustion on Discord.
