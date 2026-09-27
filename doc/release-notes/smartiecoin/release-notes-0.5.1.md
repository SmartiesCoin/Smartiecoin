# Smartiecoin Core v0.5.1

**Wallet robustness update — no consensus changes, optional for the network.**

This is a maintenance release with a single fix: wallets that contain a
damaged transaction record no longer start extremely slowly or crash on load.
No consensus rules change, there is no fork, and mining, masternodes,
governance and all existing history are unaffected.

> **Not a mandatory update.** v0.5.0 nodes and miners without wallet trouble
> are fine as-is. Recommended only for wallets that have shown slow starts
> or load crashes.

---

## What's fixed

### Damaged wallet record → silent full rescan → crash on load

A single damaged byte in a wallet's transaction database could cause, on
every start:

1. A **silent full rescan of the entire chain** — very slow (≈30 minutes
   measured on a 90 MB wallet with ~70,000 transactions on mainnet).
2. A **crash while loading the wallet**: the damaged record left an invalid
   entry behind in the wallet's in-memory transaction map, which tripped an
   internal consistency check (`assert`) while re-accepting wallet
   transactions.

The fix:

* The invalid in-memory entry is now removed immediately when the damaged
  record is detected — the crash can no longer happen.
* After the automatic recovery rescan rebuilds the wallet from the chain,
  the damaged record is **deleted from the wallet database**, so the rescan
  does not repeat on the next start.

**Net effect:** an affected wallet repairs itself on the first start with
this version — one recovery rescan, then clean, fast starts afterwards.
No manual recovery steps are needed.

## Who should update

* **Recommended** for wallets that have shown very slow starts or that
  crash while loading (large wallets and pool wallets especially).
* Optional for everyone else. No changes to consensus, PoW, governance,
  reward schedule or the network.
* No chain reset — existing history, balances and addresses are untouched.

## How to verify

* `smartiecoind --version` and `smartiecoin-qt --version` report `v0.5.1`.
* SHA256 checksums for all release assets are published with the release.

## Credits

Reported by a community miner whose pool wallet took ~25 minutes to start
and crashed on load. The detailed logs made the issue reproducible in the
lab and the fix verifiable end-to-end before shipping.
