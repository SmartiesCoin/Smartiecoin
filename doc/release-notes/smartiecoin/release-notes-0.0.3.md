# Smartiecoin Core version v0.0.3

Smartiecoin Core `v0.0.3` updates consensus economics to the final SMT schedule agreed for the reset network.

## Wallet and branding updates

- Qt resource initialization now uses `smartiecoin` naming consistently (fixes Qt link/runtime resource mismatch).
- Legacy `DisplayDashUnit` setting is migrated to `DisplaySmartiecoinUnit`.
- Receive-request placeholders were updated to Smartiecoin URI/address examples.
- Additional UI/internal naming cleanup removed remaining legacy chain/asset labels in non-locale Qt resources.
- Windows Qt resource icons now use Smartiecoin icon assets for both main/test resource entries.
- Core default config filename is now `smartiecoin.conf` (replacing legacy `bitcoin.conf` defaults in `common::args` path handling).
- Default data directories in `common::args` now align to Smartiecoin paths:
  - Windows: `%APPDATA%\\SmartiecoinCore`
  - macOS: `~/Library/Application Support/SmartiecoinCore`
  - Linux: `~/.smartiecoincore`
- Updated locale catalogs to remove remaining legacy URI/name strings in translated UI paths.
- Updated init/unit templates (`systemd`, `openrc`, `init`, and upstart examples) to use `smartiecoind` and `smartiecoin.conf` naming.
- Updated Debian packaging metadata/templates to Smartiecoin command/package names and repository URLs.
- Added Smartiecoin-named manpage/completion aliases used by Debian packaging templates.

## Consensus changes

- Initial block reward changed to `50 SMT` per block.
- Mainnet and testnet halving interval set to `1,030,596` blocks (about `715.69` days at `60s` target spacing).
- Mainnet and testnet block subsidy now enforces a strict `MAX_MONEY` cap of `100,000,000 SMT`.
- Cap logic emits normal subsidy until the cap is reached and then returns `0` subsidy for subsequent blocks.

## Related parameters

- `MAX_MONEY` remains `100,000,000 SMT`.
- Target spacing remains `60 seconds` on mainnet/testnet.
- Masternode collateral remains:
  - Regular: `15,000 SMT`
  - Evo: `75,000 SMT` (5x voting weight).

## Validation and smoke checks

- Functional smoke suite passed:
  - `rpc_masternode.py`
  - `rpc_coinjoin.py`
- Additional functional validation passed:
  - `wallet_basic.py --descriptors`
  - `mining_getblocktemplate_longpoll.py`
- Manual regtest RPC smoke passed end-to-end:
  - node startup/shutdown
  - mine `130` blocks
  - send transaction and confirm (`1` confirmation)
  - `masternode status` RPC reachable
  - `coinjoin start|status|stop` RPC path reachable
- Manual build validation passed for:
  - `smartiecoind.exe`
  - `smartiecoin-cli.exe`
  - `smartiecoin-tx.exe`
  - `smartiecoin-util.exe`
  - `smartiecoin-wallet.exe`
  - `qt/smartiecoin-qt.exe`

## Upgrade notes

- This release introduces consensus changes; all nodes must upgrade together for the reset network.
- Do not mix `v0.0.3` nodes with prior reset builds when bootstrapping the final chain.
