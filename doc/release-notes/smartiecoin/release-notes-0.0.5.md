# Smartiecoin Core version v0.0.5

Smartiecoin Core `v0.0.5` focuses on masternode onboarding in the Qt wallet with a guided setup flow.

## Highlights

- Added **MN Setup Wizard** in the **Masternodes** tab:
  - New button: `MN Setup Wizard`
  - Opens a step-by-step graphical flow for setup and registration.
- Added automatic wallet address generation in the wizard:
  - Collateral, owner, voting, payout, and fee addresses are generated from the loaded wallet.
- Added integrated BLS key generation in the wizard:
  - Uses `bls generate` internally.
  - Shows operator secret/public keys in the UI.
- Added automatic `smartiecoin.conf` operator key update:
  - Writes/updates `masternodeblsprivkey=<secret>`.
  - Detects when restart is required.
- Added one-click ProTx registration from the wizard:
  - Regular: `protx register_fund`
  - Evo: `protx register_fund_evo`
  - Displays resulting registration TxID on success.
- Added form validation and safer UX:
  - Validates required addresses and port range.
  - Wizard button is disabled when no wallet is loaded.

## Documentation updates

- Updated Qt-first Windows guide and linked wizard-first flow:
  - `doc/masternode-setup-smt-windows-qt.md`
- Updated Windows and generic MN guides to `v0.0.5` references:
  - `doc/masternode-setup-smt-windows.md`
  - `doc/masternode-setup-smt.md`

## Operational notes

- Wallet unlock is required before final registration.
- If `masternodeblsprivkey` is changed, restart wallet/node so operator service uses the new key.

## Consensus and economics

No consensus rule changes are introduced in `v0.0.5`.
This is a GUI/usability release.
