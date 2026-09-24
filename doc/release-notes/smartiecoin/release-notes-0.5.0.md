# Smartiecoin Core v0.5.0 — Korsh L3

**Scheduled consensus update (proof-of-work, known as "Korsh L3") + refreshed branding.**

This release introduces a cache-friendly YesPower configuration for new blocks.
**The change activates on Sunday, 27 September 2026, at 17:00 UTC (12:00 PM CDT)**
and is enforced **purely by block timestamp** — there is **no chain reset**, no
re-genesis or re-indexing of existing history, and nothing before the activation
time changes.

> **All miners must update before the activation.** All nodes, masternodes,
> exchanges and services are strongly recommended to update as well.

---

## What changes

### Korsh L3 — New Proof-of-Work parameters (activation: 2026-09-27 17:00 UTC)

New blocks use a lighter YesPower 1.0 configuration:

| | Before | After (v0.5.0) |
|---|---|---|
| Algorithm | YesPower 1.0 | YesPower 1.0 |
| N | 2048 | 256 |
| r | 32 | 8 |
| Working set per hash | ~8 MB | ~256 KB |
| Personalization | none | none |

How the switch happens:

* Every block carries its timestamp (`nTime`). The first block whose timestamp
  is **≥ 17:00:00 UTC on 27 Sep 2026** must be solved with the new parameters.
  Blocks before that instant keep the old parameters — bit for bit identical to
  today.
* Expected activation height: **≈ 232,480** (estimate; the rule is time-based,
  not height-based, so nodes agree even across reorgs).
* Both parameter sets are compiled into this release. Nodes and miners
  automatically select the correct one per block — no flags, no coordinated
  switchover at an exact second.

### What is "Korsh L3"? — vs. regular L3-sized Yespower, and why it is faster

> **In one sentence:** the algorithm is unchanged (Yespower); what changes is how
> much memory a single hash uses — ~**256 KB** instead of ~**8 MB** — small enough
> to be computed entirely inside the CPU's fast **L2 cache** instead of reaching
> out to slower external memory. On the same hardware, that makes mining roughly
> **25–30× faster per core.**

This update is often described as taking Smartiecoin "from L3 to L2", and the new
configuration is named **Korsh L3** because it is the exact one the Korsh project
designed and runs on its own mainnet.

The proof of work is still **Yespower 1.0** — same algorithm, same audited code
path, no new cryptography. What changes is only the *cache footprint* of a hash:
the memory a single hash works over (`128 × N × r` bytes).

* **Regular Yespower (L3)** — `N=2048, r=32`, what Smartiecoin used until this
  release and what most Yespower-based coins still use: **≈8 MB per hash**. That
  does not fit in the private L2 cache of any current CPU core, so each hash is
  served from **L3 or system memory** — bound by memory bandwidth and by how
  large the CPU's L3 cache happens to be.
* **Korsh L3** — `N=256, r=8` (the smallest configuration Yespower 1.0 accepts):
  **≈256 KB per hash**. That fits comfortably in the **L2 cache** of an ordinary
  core, so hashes are computed entirely from L2.

(L1/L2/L3 are the CPU's cache levels, ordered smallest-and-fastest (L1, private to
each core) to largest-and-slowest. A hash dataset that fits in L2 never has to
reach the slower, shared L3 or main memory — that is the entire difference here.)

**Measured speedup** — same source code, same build, one thread, hashing through
the node's own `yespower` code:

| Machine | Regular Yespower (8 MB/hash) | Korsh L3 (256 KB/hash) | Speedup |
| :--- | ---: | ---: | ---: |
| Apple Silicon | ≈0.32 kH/s | ≈8.0 kH/s | **≈25×** |
| AMD EPYC (server CPU) | ≈0.16 kH/s | ≈4.9 kH/s | **≈31×** |

Advantages:

* **≈25–30× more hashes per core on the same hardware** — out of the box.
* **Throughput scales with cores, not with memory bandwidth.** Each thread
  works from its own core's L2, so added threads add hashrate almost linearly
  (the shipped miner benchmarks ≈83 kH/s on a 30-thread server CPU) instead of
  all threads fighting over the same memory channels.
* **Mining becomes viable on weak/old hardware** — laptops, small desktops,
  even phones — which is what Korsh L3 was designed for. At 8 MB per hash those
  machines are hopelessly memory-bound.
* **Less memory traffic per hash → less power and heat per unit of work.**
* **Same algorithm and same security model.** Only parameters change; the
  network's reorg protection continues to come from **ChainLocks (masternode
  quorums)**, which this release does not touch.

Trade-off, stated plainly: a smaller working set is by definition *less*
memory-hard than the 8 MB one. That is the deliberate choice behind Korsh L3 —
drastically better CPU mining, with anti-reorg security provided by ChainLocks
rather than by the PoW footprint.

### Practical effect for miners

* Lower memory working set per hash → mining performs better on ordinary CPUs
  with regular caches (2 MB–32 MB L3), instead of scaling only with large L3/many
  channels.
* A first-party CPU miner is shipped in `contrib/smt-miner/` (solo via local
  RPC and Stratum). It picks the correct parameters by block timestamp
  automatically. **Mining with pre-0.5.0 miner builds will produce invalid
  blocks after the activation.**

#### How to run the miner (quick start)

Ready-to-run binaries are on this release page: `smt-miner-0.5.0-win64.exe`
(Windows) and `smt-miner-0.5.0-macos-arm64` (macOS).

**Pool mining** — no local node or configuration needed:

    smt-miner-0.5.0-win64.exe --stratum stratum+tcp://POOL:PORT --user YOUR_ADDRESS.rig1

**Solo mining** — against your own node (RPC enabled; default port 8282;
credentials are read from `~/.smartiecoin/smartiecoin.conf`):

    smt-miner-0.5.0-win64.exe YOUR_ADDRESS --threads 8

Useful first commands:

    smt-miner-0.5.0-win64.exe --selftest     # proves the miner matches consensus code
    smt-miner-0.5.0-win64.exe --bench 10     # measures this machine's hashrate

`-h` / `--help` prints all options. On Windows run it from Command Prompt or
PowerShell (it is a console program). On macOS, mark it executable
(`chmod +x smt-miner-0.5.0-macos-arm64`); if Gatekeeper blocks the unsigned
binary, allow it in System Settings → Privacy & Security.

Full documentation (building from source, tuning, Stratum spec):
`contrib/smt-miner/README.md` in the repository — also attached to this
release as `smt-miner-0.5.0-README.md`.

---

## What does NOT change

* **Chain history** — genesis and all existing blocks are untouched; no reset.
* **Masternodes** — collateral, payouts, deterministic MN list, quorums.
* **ChainLocks and InstantSend** — same quorum rules and security model.
* **Governance and superblocks** — unchanged schedule and amounts.
* **Addresses, wallets, explorer** — no migration needed.

The anti-reorg security of Smartiecoin continues to rely on ChainLocks
(masternode quorums), which this release does not touch.

## Bug fixes

* **`-reindex` crash fixed.** Rebuilding the block index from the block files
  could abort with an assertion (`ContextualCheckBlock: pindexPrev != nullptr`)
  because the genesis block was re-fed through the contextual checks without a
  previous block. Genesis (and any header without a known parent) now bypass the
  contextual checks, which is the correct behavior. Full-chain reindex from the
  raw block files was validated end-to-end with this build.
* Test framework: nodes started by the functional test suite now receive the
  Sapling parameter directory automatically (macOS builds do not embed the
  parameters).

## Media

New application/wallet icon set and splash screen.

---

## Upgrade instructions

1. **Stop** your node/miner.
2. **Back up** `wallet.dat` (if you run a wallet) and your `smartiecoin.conf`.
3. Replace the binaries:
   * **Windows**: replace `smartiecoind.exe` / `smartiecoin-qt.exe` (or run the
     installer).
   * **Linux**: replace `smartiecoind` / `smartiecoin-cli` / `smartiecoin-qt`.
   * **macOS**: replace the application.
4. Start the node. No reindex is required — existing data is fully compatible.
5. **Miners**: replace your old miner with `smt-miner` from
   `contrib/smt-miner/` (or your pool-provided miner updated for v0.5.0) before
   the activation. Old miners keep producing valid blocks only until the
   activation instant.

### Downgrade warning

After the activation, nodes running versions older than v0.5.0 will not accept
new blocks (they use the old parameters). If you downgrade, you will need to
either upgrade again or resynchronize from a compatible datadir. There is no
problem with pre-existing history in any direction.

---

## Notes

* The switch uses the same mechanism the chain already uses for its other
  time-based consensus parameters, so it is reorg-safe and deterministic.
* Median-time-past rules continue to apply, so the exact activation happens
  within the normal timestamp drift of a few blocks.
* Releases: Windows, Linux and macOS builds are published on the releases page.
  The `smt-miner` sources ship in the repository under `contrib/smt-miner/`.

Questions / issues: <https://github.com/SmartiesCoin/Smartiecoin/issues>
