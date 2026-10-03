# Smartiecoin Core v0.5.3

This update prevents transaction-history loss during corrupt-wallet recovery and stops destructive Berkeley DB recovery retries. It also carries wallet-load safeguards from the unpublished v0.5.2 candidate and hardens release build/publication boundaries.

**Release preparation only: final artifact validation and physical Windows mini PC QA remain pending. v0.5.2 was never published.**

## Wallet record preservation

- A transaction recovered by a rescan and written back under its correct key must survive subsequent cleanup, clean shutdown, and restart. Cleanup must not erase a valid replacement merely because that key was previously recorded as corrupt.
- Corrupt or mismatched transaction records that the rescan does not match are retained on disk, rather than discarded. They can be the only remaining copy of an unconfirmed transaction or a confirmed transaction outside the scanned range.
- **Retained does not mean recovered.** Unmatched records can remain absent from the displayed history and RPC results. They continue to cause corruption warnings and rescans on later loads; a completed rescan is not proof that every transaction was recovered. Preserve backups for further diagnosis rather than deleting these records to silence warnings.
- The functional runner includes `wallet_corruption_repair.py --database=bdb` and `wallet_corruption_repair.py --database=sqlite`. Disposable fixtures cover recovered-key persistence and byte-for-byte preservation of unmatched records across shutdowns and restarts.

## Berkeley DB: fail closed instead of destructive retry

If Berkeley DB returns `DB_RUNRECOVERY` while opening its environment, the wallet is not opened and the database environment is not reset for an automatic retry. Database logs can contain committed updates not yet present in `wallet.dat`; a compatibility message is not evidence that those logs are disposable.

Stop Smartiecoin Core and back up the **entire wallet directory**, including `wallet.dat`, the `database` directory and its logs, and other environment files, before attempting manual recovery with the software that last loaded that wallet. Keep the original backup untouched. Do not delete or move database logs to force startup, and do not treat a copy of `wallet.dat` alone as a complete recovery backup.

## Inherited wallet-load safeguards

The unpublished v0.5.2 work removes newly inserted null transaction placeholders when loading fails or deserialization throws, while preserving entries that already existed. It also retains the `CWallet` member-layout mitigation and the requirement for clean Windows object builds. These address startup robustness, but do **not** establish the cause of the previously reported Windows Qt Boost `shared_ptr` assertion (`px != 0`).

## Build and release safety

Release preparation separates untrusted build/test execution from trusted publication. Build jobs should not receive release-write credentials; publication is a separately gated trusted operation tied to the intended source revision and verified artifacts. Build bundles must be safely validated and extracted, without allowing archive paths or links to escape their destination or admitting credentials and unrelated local data. These controls do not replace wallet regression tests or final-package acceptance testing.

## Compatibility and upgrade behavior

- No consensus-rule, genesis, PoW, or wallet/database-format migration is introduced. There is no fork, chain reset, or requirement to discard blockchain data. Public legacy-wallet packages retain Berkeley DB 4.8 compatibility.
- **Linux minimum tested baseline: Ubuntu 24.04 with glibc 2.39.** This is not a claim of generic Debian support or compatibility with older glibc systems. Other distributions and runtime dependencies require separate validation of the actual release binaries.
- The version bump activates existing version-keyed startup housekeeping. On mainnet, an existing `peers.dat` can be backed up and refreshed when the current-version refresh marker is absent; `-resetpeers` can also request this behavior. Peer discovery then rebuilds the address cache. This does not reset wallets or blocks.
- Existing fast-startup behavior (`-checkblocks` negative) can clear inherited invalid-block failure flags once for the new client version when its recovery marker is absent. Normal startup verification at check level 3 or higher also has an existing failure-flag reset path. These allow blocks to be reconsidered under unchanged validation rules, not accepted unconditionally. A marker-write failure can cause the fast-startup check to repeat. **Neither operation is a consensus reset.**
- Miner updates are not required for consensus compatibility by this release.

Before replacing binaries, shut down cleanly and back up the complete wallet directory and configuration. Use freshly built v0.5.3 packages; do not relabel old artifacts. If startup still fails, preserve `debug.log` and any crash report along with the backup for diagnosis.

## Outstanding release gates

These notes describe the intended release, not a completed verification or publication:

- Build from clean object trees and verify every packaged executable reports v0.5.3; check package inventories and fresh checksums against the final source revision.
- Run both wallet-corruption regression variants and BDB failure/recovery tests, plus platform package smoke tests and the BDB 4.8 release gate.
- **Mandatory and still pending:** test the final Windows archive on the user's authorized physical Windows mini PC. Confirm host identity and record the archive hash. Use isolated disposable wallets for Qt startup/shutdown, BDB create/reopen, restart persistence, and the repaired regressions; never use real wallets. CI or another machine does not substitute for this acceptance gate.
- Keep publication on hold until the final artifacts and these gates pass.
