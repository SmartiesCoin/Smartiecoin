# Smartiecoin Core v0.5.2

**Windows Qt wallet stability update. No consensus changes.**

Two Windows users reported the Qt wallet aborting on startup in Overview with a Boost `shared_ptr` assertion (`px != 0`). No stack trace or crash dump is available, so the exact cause is unconfirmed. This update moves the new recovery-state member to the end of `CWallet`, preserving offsets of existing members, and rebuilds the Windows candidate from a clean source/object tree. This is a precautionary mitigation, not proof of the assertion's root cause.

## What changes

- Mitigates the reported startup risk by preserving existing `CWallet` member offsets and requiring a clean Windows build; the crash's exact cause remains unconfirmed.
- Keeps the v0.5.1 wallet-record recovery behavior.
- No consensus, PoW, governance, masternode, or network changes. No fork or chain reset.
- This hotfix makes no additional wallet-data changes; the v0.5.1 damaged-record recovery behavior is unchanged.
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
