# Smartiecoin Core v0.5.2

**Windows wallet GUI hotfix — rebuild after the v0.5.1 wallet layout change. No consensus changes.**

Two Windows users reported the Qt wallet aborting on startup in the Overview view with a Boost `shared_ptr` assertion (`px != 0`). The v0.5.1 Windows release had been incrementally linked after `CWallet` changed, while dependency tracking was disabled; some objects could therefore retain the previous class layout. In v0.5.2, the new recovery-state member is placed after existing `CWallet` members, and the Windows release is rebuilt from a clean source/object tree so every component uses the same layout.

## What changes

- Fixes the Windows Qt startup assertion reported in v0.5.1.
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

No reindex, rescan, wallet conversion, or manual database repair is required for this GUI/build fix. Wallets that were already recovering from a damaged transaction record retain the v0.5.1 recovery behavior.

## Credits

Thanks to the community members who reported the crash and supplied screenshots. The reports exposed a release-build consistency issue; the fix is verified against a clean Windows build before publication.
