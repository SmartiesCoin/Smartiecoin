# Smartiecoin Core version v0.0.6

Smartiecoin Core `v0.0.6` is a maintenance release focused on Windows packaging reliability and masternode wallet-mode startup fixes.

## Highlights

- Fixed wallet+masternode startup restriction:
  - Removed the hard startup error that blocked running a masternode with wallet enabled.
  - `-masternodeblsprivkey` no longer forces a wallet-disabled startup path.
- Version metadata corrected to `0.0.6` across Windows binaries:
  - `smartiecoin-qt.exe`
  - `smartiecoind.exe`
  - `smartiecoin-cli.exe`
  - `smartiecoin-tx.exe`
  - `smartiecoin-util.exe`
  - `smartiecoin-wallet.exe`
- Windows portable package updated with required runtime dependencies:
  - Includes Qt platform/plugins and mingw runtime DLLs.
  - Includes previously missing DLLs reported by users (`libminiupnpc.dll`, `libevent-7.dll`, `libgmp-10.dll`, `libgcc_s_seh-1.dll`, and related runtime libs).

## Smoke test summary (Windows portable)

- Qt startup check: PASS
- CLI/daemon version check (`v0.0.6`): PASS
- Regtest wallet flow (create wallet, mine, send tx): PASS
- Masternode wallet-mode startup regression check (no old startup block message, wallet loads with `-masternodeblsprivkey`): PASS

## Consensus and economics

No consensus rule changes in `v0.0.6`.
This is a stability/packaging release.
