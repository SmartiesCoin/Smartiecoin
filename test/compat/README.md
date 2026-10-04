# Optional / Sapling serialization compatibility gate

Run from the repository root with a C++20 compiler and the project's development
headers available. Set `CXX` and `CPPFLAGS` for your toolchain. Build outside the
source tree:

```sh
CPPFLAGS=-I/opt/homebrew/include python3 test/compat/run.py \
  --build-dir "$HOME/.hermes/cache/scratch/smt-compat" \
  --rust-lib "$PWD/build-v050/target/release/librustzcash.a"
```

The Rust archive path is an example; pass your actual built project archive.
Without `--rust-lib`, the runner explicitly reports the Pedersen hashing test as
**NOT RUN**. It still runs the serialization-boundary, KeyIO, and transaction
fixtures. Assertions are enabled regardless of the caller's release settings.
The runner writes exact compile/run commands, exit codes, and output to
`results.json` in the build directory. It never regenerates expected output.

## Provenance and coverage

`boost_baseline.json` contains output captured by compiling and executing the
pre-migration Boost-backed production serializers at
`eba2fea3c687c4a54ae95dc48d71bfb758197f54`, with the concurrent wallet-service
changes preserved. It is not generated from the std candidate. Later expanded
fixtures were also executed against the saved pre-migration headers before
comparison with the candidate. No replacement serializer or crypto function is
linked by these tests.

- `std_types.cpp`: the Optional alias and all three variant alternatives/order.
- `wire.cpp`: all 256 bool tags; every incomplete present uint32 encoding for
  every nonzero tag, with empty and populated destinations; partial multi-field
  payload failure retention; nested/vector encodings; all bytes of the 43-byte
  address and 169-byte keys; roundtrip and every truncation offset; default
  invalid alternative, pointer misses, and the unusual existing
  `InvalidEncoding < InvalidEncoding` result.
- `keyio.cpp`: real payment/viewing/spending visitors and validity functions,
  optional payment wrapper, invalid strings, wrong HRP/type, checksum and padding
  rejection, BECH32M rejection. Test-only chain parameters provide fixed HRPs.
- `sapling_serialization.cpp`: actual Sapling tree/witness decoding and rewrite,
  optional cursor and parent vector, malformed-tree rejection, shielded payload,
  version-4 and legacy-version-3 `CMutableTransaction` serialization, spend/output
  vectors, and the serialized key/value shapes used by the Sapling WalletBatch
  writers (including the real encrypted-record checksum operation). Every fixture
  is roundtripped and tested at every truncation offset. The runner checks DB tag
  literals against the actual walletdb.cpp definitions.
- `merkle.cpp`: actual production tree/witness append, Pedersen hashing through
  the project's Rust FFI, root equality, and serialization/rewrite over fifteen
  successive states crossing parent/cursor boundaries. Both the serialized state
  and root bytes must equal the captured Boost output.

## Boundaries

These are synthetic public byte/field-element fixtures, not derived spendable
keys, validated transactions, or wallet secrets. The wallet-record checks exercise
serialization shapes, not database creation, actual WalletBatch I/O, encryption,
restart persistence, or RPC transport. They are standalone maintained tests, not
automatically part of the autotools `test_dash` target.

Failed **payload decoding before assignment** retains the old optional value.
This does not promise stream-cursor rollback, rollback of a containing object,
or strong exception safety if `T` itself throws during assignment.

This gate is not release approval. Release builds require Berkeley DB **4.8**;
a native diagnostic compile using another installed BDB version does not satisfy
that requirement. Full project builds, supported-platform CI, and wallet lifecycle
acceptance remain separate gates.
