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

### Practical effect for miners

* Lower memory working set per hash → mining performs better on ordinary CPUs
  with regular caches (2 MB–32 MB L3), instead of scaling only with large L3/many
  channels.
* A first-party CPU miner is shipped in `contrib/smt-miner/` (solo via local
  RPC and Stratum). It picks the correct parameters by block timestamp
  automatically. **Mining with pre-0.5.0 miner builds will produce invalid
  blocks after the activation.**

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
