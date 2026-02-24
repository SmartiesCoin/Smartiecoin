# Smartiecoin Core version v0.0.3

Smartiecoin Core `v0.0.3` updates consensus economics to the final SMT schedule agreed for the reset network.

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

- Subsidy unit test vectors were updated for:
  - 50 SMT start
  - first/second halving boundaries
  - cap boundary behavior.
- Runtime smoke checks passed for:
  - daemon startup/shutdown
  - regtest mining
  - wallet transaction flow
  - CoinJoin RPC start/status/stop
  - basic ProTx RPC access (`protx list`).
- Additional note: upstream full `test_dash` build is currently blocked by missing `src/test/script_p2sh_tests.cpp` in the repository baseline.

## Upgrade notes

- This release introduces consensus changes; all nodes must upgrade together for the reset network.
- Do not mix `v0.0.3` nodes with prior reset builds when bootstrapping the final chain.
