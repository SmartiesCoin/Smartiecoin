# Smartiecoin Core version v0.0.8

Smartiecoin Core `v0.0.8` is a maintenance release focused on masternode ProTx command compatibility for external hosting integrations and version metadata cleanup.

## Highlights

- Fixed ProTx command-path compatibility on Smartiecoin's legacy-BLS consensus era:
  - `protx register`
  - `protx register_prepare`
  - `protx register_submit`
- Added automatic legacy/basic BLS normalization when parsing operator public keys for ProTx registration flows.
- Kept wallet wizard behavior unchanged while aligning command-path behavior with the same compatibility expectations.
- Updated version metadata to `0.0.8` for:
  - Core package version (`configure.ac`)
  - Qt bundle version (`share/qt/Info.plist`)
  - Windows installer/product metadata (`share/setup.nsi`)

## Smoke test summary

- `contrib/smoke/smoke-release-mn-privacy.ps1 -Profile quick`: PASS
  - `rpc_masternode.py`: PASS
  - `rpc_coinjoin.py`: PASS
- Regtest ProTx command-path smoke (manual scripts):
  - `protx register` with external collateral: PASS
  - `protx register_prepare` + `protx register_submit`: PASS
  - Regular and Evo registration flow sanity (`register_fund`, `register_fund_evo`): PASS

## Consensus and economics

No consensus, emission, or chain reset changes are introduced in `v0.0.8`.
This is a compatibility and reliability release.
