# smt-miner

Solo and pool CPU miners for Smartiecoin (Yespower). All of them hash with the node's own `yespower_hash()` and
only build and submit ordinary blocks or shares, so they are fully compatible with the official consensus rules.

| Tool | What it is |
| :--- | :--- |
| `smt-miner` (C, recommended) | Native miner. Solo mode (talks to your node over RPC) **and Stratum client** for pools. No Python needed, threads pinned to cores on Linux/Windows. On x86/SSE2 it builds in a two-way interleaved Yespower kernel (about +35 % hashrate on modern server CPUs) plus the reference kernel; at start-up it proves both identical and benchmarks them, then mines with the faster one on your CPU. |
| `smt_miner.py` + `libsmt.so` | Python solo miner with a native nonce scanner; easier to read and modify. |
| `smt_stratum_pool.py` | **Reference Stratum pool** for testing and for pool developers. Not a production pool. |

## PoW parameters and the Korsh L3 fork (v0.5.0 — read this first)

The node chooses the Yespower parameter set from the block's `nTime` (`src/crypto/yespower/yespower.c`):

| `nTime` | Parameters |
| :--- | :--- |
| `>= 1790528400` (2026-09-27 17:00:00 UTC, `consensus.nSMTv050PowTime`) | v0.5.0: `N=256, r=8`, no personalization (cache-tuned) |
| `> 1546539305` and before the fork | yespower 1.0: `N=2048, r=32`, no personalization |
| older than that | yespower 0.5: `N=4096, r=32`, personalization `WaviBanana` |

Everything here compiles the node's `yespower.c` (directly, or into `libsmt.so`), so it hashes with consensus
code — but that file's parameter switch is armed from outside, so `smt-miner`, `smt_miner.py` and
`smt_stratum_pool.py` all call `yespower_set_v050_fork_time(1790528400)` at start-up.
Without that call every post-fork hash would silently use the old parameters and no block would be accepted.

`smt-yp2.c` (the two-way kernel) implements **only** the post-fork parameters and declines anything else, in which
case the miner falls back to the reference function, which knows all three sets. Nothing else in the miner cares
which set is in use.

## Build

```bash
contrib/smt-miner/build.sh
```

It builds `smt-miner` and — when the compiler has OpenMP — `libsmt.so`, used by the Python miner and the
reference pool. Needs `gcc`/`clang`, libcurl, jansson and OpenSSL development files:

* Debian/Ubuntu: `apt install gcc libcurl4-openssl-dev libjansson-dev libssl-dev`
* macOS: `xcode-select --install` and `brew install jansson openssl@3` (libcurl comes with the SDK)
* MSYS2: `pacman -S mingw-w64-ucrt-x86_64-{gcc,curl,jansson,openssl}`

On macOS with Homebrew, `build.sh` finds jansson and the keg-only OpenSSL automatically (`HOMEBREW_PREFIX`,
`/opt/homebrew`, `/usr/local`, or your own `CPPFLAGS`/`LDFLAGS`).

**The two-way kernel is x86/SSE2 only by design.** On any other CPU (for example Apple Silicon) `build.sh` builds
the miner with `-DSMT_NO_YP2` and the reference kernel is used, one hash at a time; `--selftest` then prints
`two-way kernel: not available in this build`. On x86 the two-way kernel is compiled in and used only when it is
proven identical to the reference at start-up, and then benchmarks both and mines with whichever is faster
on this CPU (on modern parts that is usually the two-way kernel; on older small-L2 CPUs it is usually the
reference one). `--ways 1|2` forces a specific kernel.

## Solo mining

```bash
bin/smartiecoin-cli getnewaddress
contrib/smt-miner/smt-miner <address> --threads 8
```

Options: `--threads N` (default: CPU count minus 2), `--conf FILE`, `--rpc URL`, `--no-pin`, `--bench [seconds]`,
`--selftest`, `--ways 1|2` (kernel; default: auto — both are proven identical first, then the faster one is picked).
RPC credentials come from `smartiecoin.conf` (`~/.smartiecoin/smartiecoin.conf` and
`~/.smartiecoincore/smartiecoin.conf` are tried by default, otherwise pass `--conf`); the default RPC port is
**8282** (mainnet), i.e. `http://127.0.0.1:8282/`.

## Pool mining (Stratum v1)

```bash
contrib/smt-miner/smt-miner --stratum stratum+tcp://pool.example.org:3333 \
    --user <your address>.rig1 --pass x --threads 8
```

No local node or `smartiecoin.conf` is needed in this mode. The miner reconnects automatically, honours
`mining.set_difficulty`, `mining.set_extranonce` and `client.reconnect`, and reports accepted/rejected shares.

### Protocol conventions (a pool must match these)

| Item | Convention |
| :--- | :--- |
| Difficulty 1 | `0x0000ffff00..00` (the scrypt/Yespower convention). Use `--diff1 bitcoin` for `0x00000000ffff00..00`. Share target = diff1 / difficulty. |
| `mining.notify` | `[job_id, prevhash, coinb1, coinb2, merkle_branch, version, nbits, ntime, clean]` |
| `prevhash` | The previous block hash with every 4-byte word byte-swapped (standard Stratum v1). |
| `coinbase` | `coinb1 + extranonce1 + extranonce2 + coinb2`; its double-SHA256 is folded with `merkle_branch` to get the root. |
| `version`, `nbits`, `ntime` | Hex of the numeric header field (`%08x`). |
| `mining.submit` | `[user, job_id, extranonce2, ntime, nonce]`, `ntime` and `nonce` as `%08x`. |

Whether a share is also a block is decided by the node (`submitblock`) on the pool side; the miner never
needs to know.

### Try it locally with the reference pool

```bash
# terminal 1: needs a running node (with at least one peer, see below)
contrib/smt-miner/smt_stratum_pool.py <pool address> --listen 127.0.0.1:3333 --ease 16
# terminal 2
contrib/smt-miner/smt-miner --stratum stratum+tcp://127.0.0.1:3333 --user <address>.test
```

`--ease N` makes shares N times easier than a block so that shares show up often; `--ease 1` makes every
share a block. The reference pool validates shares with the node's `yespower_hash()` and submits blocks with
`submitblock`. It has no accounting, payouts, authentication or DoS protection.

## Performance and tuning

The two-way interleaved kernel and the numbers below come from the x86 build this miner was ported from (same
kernel, same post-fork parameters); they are representative of x86 servers, not of ARM.

```
contrib/smt-miner/smt-miner --bench 15 --threads 2      # Apple Silicon (arm64), reference kernel, post-fork params
threads=2 ways=1 seconds=15.0 hashrate=4.4 kH/s (2.22 kH/s per thread)
hugepages: not applicable on this OS (Linux-only accounting)
selftest digest: 0259fa5d24fff45e5c1cfd8d0dc010e22df581424f5d6e64331e8fdb8f7731cb
```

* `--bench` hashes synthetic headers with `nTime` at/after the fork, so it always measures the post-fork
  parameters (`N=256, r=8`, about 256 KB per hash). Pre-fork headers (`N=2048, r=32`) are much slower per hash.
* Every build variant must print the same self-test digest, i.e. the same hashes as the consensus code.

### Two-way interleaved kernel (x86 only): about +35 %

One Yespower hash is a long chain of dependent steps: each pwxform round needs the previous round's result before it
can index the S-boxes, so a core mostly waits (about 1.75 instructions per cycle on a CPU that can issue several
times more). `smt-yp2.c` computes **two independent hashes at once** and weaves their instructions together, so the
core has twice as many independent chains to overlap.

| Kernel (AMD EPYC 9554, x86, post-fork parameters) | 1 thread | 8 threads | 30 threads |
| :--- | ---: | ---: | ---: |
| Reference, one hash at a time (`--ways 1`) | 2.05 kH/s | 16.4 kH/s | 61.6 kH/s |
| Two-way interleaved (`--ways 2`, default where built) | 2.79 kH/s | 22.3 kH/s | 83.0 kH/s |

How it stays correct: the kernel does not replace consensus code. It includes the node's own `yespower-opt.c`
(unmodified) and each of the two instances executes exactly the reference's operations in the reference's order.
Every start-up the miner compares the two-way kernel with the reference on random post-fork headers; it can only be
used when the results are identical (otherwise it silently falls back to one hash at a time), and then both kernels
are speed-probed and the faster one is picked for this CPU.

On older Xeons the reference kernel wins: measured on a dual E5-2697 v2 (Ivy Bridge, 256 KB L2) 41.7 kH/s (`--ways 1`)
vs 33.8 kH/s (`--ways 2`) at 8 threads — the auto-tune picks `--ways 1` there. The EPYC numbers above are the modern
opposite case. `--selftest` runs a bigger
check. If the node's Yespower parameters ever change, the check fails and the miner keeps working with the reference
kernel until `smt-yp2.c` is updated.

### Other notes

- **Use every core.** Throughput scales linearly with threads (no memory contention between threads).
- **The x86 instruction set level does not matter.** The kernel is written with 128-bit vectors, so AVX2 or
  CPU-specific flags give no measurable speed-up. `-march=native` (the default) is fine; for a binary that must run
  on other x86 machines use `SMT_CFLAGS="-O3 -march=x86-64-v2"`.
- **Huge pages are optional and Linux only.** The upstream allocator only requests huge pages for regions of 12 MB
  or more, so a stock build never uses them for these 256 KB regions. `SMT_HUGEPAGES=1 ./build.sh` builds a miner
  that does, and `setup-hugepages.sh` reserves the pages. The gain was about +0.4 % on x86; it can be larger on CPUs
  with small TLBs. Reserved pages are removed from the memory available to everything else.

### macOS specifics

- No thread affinity API exists, so threads are not pinned; `--no-pin` is accepted and does nothing.
- The huge-page line of `--bench` reports `not applicable on this OS`; `setup-hugepages.sh` and
  `SMT_HUGEPAGES=1` refuse to run outside Linux.
- `libsmt.so` (and therefore the Python miner) needs an OpenMP-enabled compiler: `brew install libomp`.
  The C miner does not need it.
- `-march=native` maps to `-mcpu=native` under Apple clang; both work.

Measure your own machine (stop other miners first):

```bash
contrib/smt-miner/smt-miner --bench 10 --threads 8 [--ways 1|2]   # hashrate, kernel, huge page use, digest
contrib/smt-miner/bench.sh                                        # compares build variants and both kernels
contrib/smt-miner/smt-miner --selftest                            # digest + two-way vs reference check
```

## Verification

* `smt-miner --selftest` prints one line on stdout: the SHA-256 of the hashes of 64 fixed headers, and on stderr the
  result of the two-way-kernel-versus-reference check. The headers carry a post-fork `nTime`, so the digest proves
  the post-fork parameter path. Two builds that print the same digest compute the same hashes as the node.
* The PoW-switch vectors (pre-fork, at-fork, fork disabled) for headers built as `input[i] = i*7+1` (i < 76) with
  `nTime` at bytes 68..71 and `T = 1790438400` are, per the node's own `yespower.c`:

  | Case | Hash |
  | :--- | :--- |
  | `t = T-1`, fork armed at `T` (legacy, 2048/32) | `d7a14e532ce6b1c08b6c4421e22097aa8de6921d9f9c771f4c8caf2f1ac230fd` |
  | `t = T`, fork armed at `T` (v0.5.0, 256/8) | `d32f82bb81aa9c3a97ea94e03add1ee1e7e1aa4c84cc98ae1ce5020c1b0fbdec` |
  | `t = T`, fork disabled (legacy, 2048/32) | `e7e062d5dfc0b3201cb8e7ae9f4d5b0ebb62154c6311c076392a657e99cbf5fc` |

  The miner's own hashing path reproduces these three values byte for byte.

## Node requirements

* `getblocktemplate` refuses to answer while the node is in initial block download or has **no peers**. On a
  brand-new chain with no other Smartiecoin nodes, run a **second local node** connected to the first and set
  `maxtipage=999999999` on the first node until blocks are produced.
* The template must not require masternode or superblock payments: the miner refuses such templates instead of
  building an invalid coinbase.
* Templates without segwit expose transactions as `hash` (not `txid`); the miner and the reference pool handle both.

## Why not `cpuminer-opt`?

Its Yespower does not implement this coin's parameter sets, so its hashes are rejected (`high-hash`). The miner here
calls the node's own `yespower_hash()`, which also means it follows the parameter switch automatically.
