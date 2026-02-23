# Smartiecoin Port Plan to Bitcoin Core v30.2

## Objective

Move the Smartiecoin codebase onto Bitcoin Core `v30.2` while preserving:

- YesPower PoW.
- Masternodes and LLMQ stack.
- Optional privacy flow (CoinJoin), not forced by default.
- Reset chain parameters already applied for a fresh network.

## Current Status (Session Snapshot)

- Gap tooling created:
  - `contrib/devtools/porting/generate_gap_report.py`
  - `contrib/devtools/porting/generate_phase2_worklist.py`
- Reports generated:
  - `doc/porting/bitcoin-v30.2-gap-report.md`
  - `doc/porting/bitcoin-v30.2-gap-report.json`
  - `doc/porting/bitcoin-v30.2-phase2-runtime-worklist.md`
- First staged code import from upstream `v30.2` completed:
  - 21 new files copied into `src/common/` (not yet wired into build graph).
- Validation after import:
  - `make -j4` passed
  - `test/functional/test_runner.py --jobs=1 mining_basic.py` passed

## Constraints

- Do not lose Smartie-specific consensus and network behavior.
- Keep wallet and RPC behavior stable for existing operators.
- Keep test coverage runnable on Windows and Unix-like environments.

## Structured Phases

### Phase 1: Baseline and Gap Mapping

- Build and test current Smartie baseline.
- Generate an automated gap report against `v30.2`.
- Identify three buckets:
  - Upstream core drift to port.
  - Smartie-specific modules to preserve.
  - Mixed files requiring manual merge.

Deliverables:

- `doc/porting/bitcoin-v30.2-gap-report.md`
- `doc/porting/bitcoin-v30.2-gap-report.json`

### Phase 2: Core Runtime Alignment

- Port low-risk shared runtime layers first:
  - `src/common`
  - `src/util`
  - `src/kernel` and generic node wiring
- Keep feature islands isolated:
  - YesPower integration points
  - Masternode/LLMQ/ChainLock/InstantSend stack
  - CoinJoin stack

Success criteria:

- Clean build.
- Unit test suite still green.
- Functional tests for wallet/mining/P2P still green.

### Phase 3: Network and RPC Reconciliation

- Reconcile P2P, net processing, and RPC surfaces with `v30.2`.
- Preserve existing Smartie RPCs for masternodes and privacy.
- Add compatibility shims only where unavoidable.

Success criteria:

- P2P functional tests pass.
- LLMQ and masternode functional tests pass.
- CoinJoin RPC tests pass.

### Phase 4: Wallet and Privacy Stabilization

- Align wallet internals with upstream while keeping CoinJoin optional behavior.
- Validate descriptor and legacy wallet paths.

Success criteria:

- Wallet functional tests pass in both modes.
- CoinJoin flow remains optional and operational.

### Phase 5: Release Hardening

- Final consensus and chainparams validation.
- Reproducible build checks.
- Final regression sweep and release docs.

## Execution Commands

Generate the gap report:

```powershell
py -3 contrib/devtools/porting/generate_gap_report.py `
  --smartie-root . `
  --bitcoin-root .upstream/bitcoin-30.2 `
  --output-md doc/porting/bitcoin-v30.2-gap-report.md `
  --output-json doc/porting/bitcoin-v30.2-gap-report.json
```

Baseline build and tests:

```powershell
# Use your configured build shell/environment
make -j4
make check -j4
py -3 test/functional/test_runner.py --jobs=2 wallet_basic.py mining_basic.py p2p_compactblocks.py feature_llmq_signing.py rpc_coinjoin.py
```
