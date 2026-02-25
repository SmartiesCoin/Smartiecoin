# Smartiecoin Core version v0.0.4

Smartiecoin Core `v0.0.4` focuses on masternode usability for miners/operators who want an all-in-one setup.

## Highlights

- Enabled **single-wallet masternode mode** in the same node instance:
  - `-masternodeblsprivkey` no longer auto-forces `-disablewallet=1`.
  - Operators can now run wallet + masternode in one process when desired.
- Updated ProTx RPC help collateral amounts to match current consensus:
  - Regular masternode collateral: `15,000 SMT`
  - EvoNode collateral: `75,000 SMT`
- Added an English setup guide:
  - `doc/masternode-setup-smt.md`
  - Includes both split hot/cold architecture and single-wallet mode notes.
  - Direct link: https://github.com/SmartiesCoin/Smartiecoin/blob/main/doc/masternode-setup-smt.md
- Added a Windows step-by-step masternode guide:
  - `doc/masternode-setup-smt-windows.md`
  - Direct link: https://github.com/SmartiesCoin/Smartiecoin/blob/main/doc/masternode-setup-smt-windows.md
- Added a Windows Qt-first masternode guide (minimal commands):
  - `doc/masternode-setup-smt-windows-qt.md`
  - Direct link: https://github.com/SmartiesCoin/Smartiecoin/blob/main/doc/masternode-setup-smt-windows-qt.md

## Security note

Single-wallet mode is convenient, but has a larger risk surface than split hot/cold architecture.
For larger balances, split architecture remains recommended.

## Consensus and economics

No consensus rule changes are introduced in `v0.0.4`.
This release is operational/usability-focused.
