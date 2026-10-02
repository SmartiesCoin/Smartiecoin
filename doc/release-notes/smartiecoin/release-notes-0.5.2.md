# Smartiecoin Core v0.5.2

**Windows Qt wallet stability update. No consensus changes.**

Two Windows users reported the Qt wallet aborting on startup in Overview with a Boost `shared_ptr` assertion (`px != 0`). No stack trace or crash dump is available, so the exact cause remains unconfirmed. v0.5.2 adds wallet-load safeguards: it removes newly inserted placeholders when a load returns false or transaction deserialization throws, while preserving any transaction entry that already existed. It also retains the `CWallet` member-layout mitigation and requires a clean Windows object build. Regression tests cover the exception cleanup and preservation of an existing entry; none of these changes proves the cause of the reported assertion.

## What changes

- Mitigates the reported startup risk by preserving existing `CWallet` member offsets and requiring a clean Windows build; the crash's exact cause remains unconfirmed.
- Removes a temporary null transaction placeholder when transaction-value deserialization throws, then lets the existing corrupt-record recovery/rescan path continue.
- Preserves a pre-existing wallet transaction when a duplicate load fails; cleanup removes only placeholders inserted by that load.
- Preserves the v0.5.1 stored-key/value mismatch recovery behavior.
- Refreshes the statically linked libsodium dependency to the upstream 1.0.22 stable-branch snapshot; no consensus or wallet-format change.
- No consensus, PoW, governance, masternode, or network changes. No fork or chain reset.
- The exception-safety change does not rewrite `wallet.dat` or alter the Berkeley DB format; it preserves the existing rescan recovery flow.
- Miner software is unchanged; v0.5.1 miner kits remain compatible and do not need an update.

## Who should update

- **Recommended for Windows Qt wallet users on v0.5.1**, especially anyone who saw the Boost `px != 0` assertion.
- Linux and macOS users may update for version alignment; no platform-specific wallet behavior changes are intended there.
- Nodes and miners do not need to update for consensus or network compatibility.

## Verification

Release artifacts must be built from clean object trees. The Windows artifact must pass the BDB 4.8 release gate, wallet QA, and an isolated Qt/regtest startup-and-shutdown smoke test. Each executable must report `v0.5.2`; release checksums are published in `SHA256SUMS.txt`.

## Upgrade

1. Close Smartiecoin Core.
2. Back up `wallet.dat` and `smartiecoin.conf` as a precaution.
3. Replace the Windows binaries with the v0.5.2 package and start the wallet normally.

No chain reset or consensus migration is part of this update, and it does not intentionally rewrite wallet data. Back up `wallet.dat` before upgrading. If the assertion recurs, preserve the crash report and `debug.log` for diagnosis; do not manually edit or reset the wallet.

## Credits

Thanks to the community members who reported the crash and supplied screenshots. The screenshots identify the symptom but not the failing call; the underlying cause remains unconfirmed.
