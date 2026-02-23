# Smartiecoin vs Bitcoin v30.2 Gap Report

- Generated: `2026-02-23T05:02:14Z`
- Smartie root: `C:\Dev\Smartiecoin`
- Bitcoin root: `C:\Dev\Smartiecoin\.upstream\bitcoin-30.2`

## Summary

| Metric | Count |
| --- | ---: |
| Indexed files in Smartie | 3507 |
| Indexed files in Bitcoin v30.2 | 2443 |
| Smartie-only files | 1825 |
| Bitcoin-only files | 761 |
| Common files (same content) | 59 |
| Common files (different content) | 1623 |

## High-Impact Areas

### Common files with content drift by subdirectory

| Subdirectory | Diff files |
| --- | ---: |
| `src/test` | 239 |
| `test/functional` | 235 |
| `src/qt` | 154 |
| `src/leveldb` | 147 |
| `src/(root)` | 136 |
| `src/secp256k1` | 124 |
| `src/wallet` | 68 |
| `src/util` | 47 |
| `src/minisketch` | 45 |
| `doc/(root)` | 40 |
| `src/bench` | 39 |
| `src/crypto` | 39 |
| `src/crc32c` | 31 |
| `src/univalue` | 31 |
| `src/node` | 29 |
| `src/rpc` | 28 |
| `test/lint` | 19 |
| `src/script` | 17 |
| `src/consensus` | 10 |
| `src/ipc` | 10 |
| `src/policy` | 10 |
| `src/zmq` | 10 |
| `src/index` | 9 |
| `src/interfaces` | 8 |
| `src/support` | 8 |
| `contrib/devtools` | 7 |
| `contrib/guix` | 7 |
| `src/compat` | 7 |
| `src/init` | 7 |
| `contrib/seeds` | 5 |
| `depends/(root)` | 5 |
| `contrib/linearize` | 4 |
| `contrib/tracing` | 4 |
| `contrib/verify-commits` | 4 |
| `src/common` | 4 |
| `src/primitives` | 4 |
| `doc/design` | 3 |
| `doc/policy` | 3 |
| `test/(root)` | 3 |
| `contrib/(root)` | 2 |
| `contrib/macdeploy` | 2 |
| `contrib/message-capture` | 2 |
| `contrib/qos` | 2 |
| `contrib/shell` | 2 |
| `contrib/testgen` | 2 |
| `share/rpcauth` | 2 |
| `src/kernel` | 2 |
| `contrib/init` | 1 |
| `contrib/windeploy` | 1 |
| `contrib/zmq` | 1 |
| `share/(root)` | 1 |
| `share/qt` | 1 |
| `src/logging` | 1 |
| `test/fuzz` | 1 |

### Smartie-only files by top-level area

| Area | Files |
| --- | ---: |
| `src` | 1596 |
| `test` | 90 |
| `doc` | 75 |
| `contrib` | 38 |
| `build-aux` | 17 |
| `share` | 5 |
| `depends` | 4 |

### Bitcoin-only files by top-level area

| Area | Files |
| --- | ---: |
| `src` | 474 |
| `doc` | 118 |
| `test` | 100 |
| `cmake` | 37 |
| `contrib` | 29 |
| `share` | 2 |
| `depends` | 1 |

## Feature Preservation Surfaces

### `yespower_pow`

- Smartie-only matches: `44`
- Diverged common matches: `0`

Smartie-only sample:
- `src/crypto/x11/aes.cpp`
- `src/crypto/x11/aes.h`
- `src/crypto/x11/arm_crypto/echo.cpp`
- `src/crypto/x11/arm_crypto/shavite.cpp`
- `src/crypto/x11/arm_neon/echo.cpp`
- `src/crypto/x11/blake.c`
- `src/crypto/x11/bmw.c`
- `src/crypto/x11/cubehash.c`
- `src/crypto/x11/dispatch.cpp`
- `src/crypto/x11/dispatch.h`
- `src/crypto/x11/echo.cpp`
- `src/crypto/x11/groestl.c`
- `src/crypto/x11/jh.c`
- `src/crypto/x11/keccak.c`
- `src/crypto/x11/luffa.c`
- `src/crypto/x11/shavite.cpp`
- `src/crypto/x11/simd.c`
- `src/crypto/x11/skein.c`
- `src/crypto/x11/sph_blake.h`
- `src/crypto/x11/sph_bmw.h`
- `src/crypto/x11/sph_cubehash.h`
- `src/crypto/x11/sph_echo.h`
- `src/crypto/x11/sph_groestl.h`
- `src/crypto/x11/sph_jh.h`
- `src/crypto/x11/sph_keccak.h`
- `src/crypto/x11/sph_luffa.h`
- `src/crypto/x11/sph_shavite.h`
- `src/crypto/x11/sph_simd.h`
- `src/crypto/x11/sph_skein.h`
- `src/crypto/x11/sph_types.h`
- `src/crypto/x11/ssse3/echo.cpp`
- `src/crypto/x11/util/consts_aes.hpp`
- `src/crypto/x11/util/util.hpp`
- `src/crypto/x11/x86_aesni/echo.cpp`
- `src/crypto/x11/x86_aesni/shavite.cpp`
- `src/crypto/yespower/insecure_memzero.h`
- `src/crypto/yespower/sha256.c`
- `src/crypto/yespower/sha256.h`
- `src/crypto/yespower/sysendian.h`
- `src/crypto/yespower/yespower-opt.c`
- `src/crypto/yespower/yespower-platform.c`
- `src/crypto/yespower/yespower.c`
- `src/crypto/yespower/yespower.h`
- `src/hash_x11.h`

### `masternodes_llmq`

- Smartie-only matches: `993`
- Diverged common matches: `0`

Smartie-only sample:
- `doc/evodb-verify-repair.md`
- `doc/instantsend.md`
- `doc/masternode-budget.md`
- `src/active/masternode.cpp`
- `src/active/masternode.h`
- `src/active/quorums.cpp`
- `src/active/quorums.h`
- `src/chainlock/chainlock.cpp`
- `src/chainlock/chainlock.h`
- `src/chainlock/clsig.cpp`
- `src/chainlock/clsig.h`
- `src/chainlock/handler.cpp`
- `src/chainlock/handler.h`
- `src/chainlock/signing.cpp`
- `src/chainlock/signing.h`
- `src/dashbls/.gitignore`
- `src/dashbls/CMakeLists.txt`
- `src/dashbls/MANIFEST.in`
- `src/dashbls/Makefile`
- `src/dashbls/Makefile.am`
- `src/dashbls/Makefile.in`
- `src/dashbls/README.md`
- `src/dashbls/aclocal.m4`
- `src/dashbls/apple.rust.deps.sh`
- `src/dashbls/autogen.sh`
- `src/dashbls/build-aux/ltmain.sh`
- `src/dashbls/build-aux/m4/ax_check_compile_flag.m4`
- `src/dashbls/build-aux/m4/ax_check_link_flag.m4`
- `src/dashbls/build-aux/m4/ax_cxx_compile_stdcxx.m4`
- `src/dashbls/build-aux/m4/ax_pthread.m4`
- `src/dashbls/build-aux/m4/libtool.m4`
- `src/dashbls/build-aux/m4/ltoptions.m4`
- `src/dashbls/build-aux/m4/ltsugar.m4`
- `src/dashbls/build-aux/m4/ltversion.m4`
- `src/dashbls/build-aux/m4/lt~obsolete.m4`
- `src/dashbls/cmake_modules/BrewHelper.cmake`
- `src/dashbls/cmake_modules/Findgmp.cmake`
- `src/dashbls/cmake_modules/Findsodium.cmake`
- `src/dashbls/configure.ac`
- `src/dashbls/depends/catch2/CMakeLists.txt`
- `src/dashbls/depends/catch2/include/catch2/catch.hpp`
- `src/dashbls/depends/mimalloc/.gitattributes`
- `src/dashbls/depends/mimalloc/.gitignore`
- `src/dashbls/depends/mimalloc/CMakeLists.txt`
- `src/dashbls/depends/mimalloc/SECURITY.md`
- `src/dashbls/depends/mimalloc/azure-pipelines.yml`
- `src/dashbls/depends/mimalloc/bin/readme.md`
- `src/dashbls/depends/mimalloc/cmake/JoinPaths.cmake`
- `src/dashbls/depends/mimalloc/cmake/mimalloc-config-version.cmake`
- `src/dashbls/depends/mimalloc/cmake/mimalloc-config.cmake`
- `src/dashbls/depends/mimalloc/contrib/docker/readme.md`
- `src/dashbls/depends/mimalloc/contrib/vcpkg/portfile.cmake`
- `src/dashbls/depends/mimalloc/contrib/vcpkg/readme.md`
- `src/dashbls/depends/mimalloc/contrib/vcpkg/vcpkg-cmake-wrapper.cmake`
- `src/dashbls/depends/mimalloc/contrib/vcpkg/vcpkg.json`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-c5-18xlarge-2020-01-20-a.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-c5-18xlarge-2020-01-20-b.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-c5-18xlarge-2020-01-20-rss-a.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-c5-18xlarge-2020-01-20-rss-b.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-r5a-1.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-r5a-12xlarge-2020-01-16-a.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-r5a-12xlarge-2020-01-16-b.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-r5a-2.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-r5a-rss-1.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-r5a-rss-2.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-spec-rss.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-spec.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-z4-1.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-z4-2.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-z4-rss-1.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2020/bench-z4-rss-2.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-amd5950x-2021-01-30-a.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-amd5950x-2021-01-30-b.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-c5-18xlarge-2021-01-30-a.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-c5-18xlarge-2021-01-30-b.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-c5-18xlarge-2021-01-30-rss-a.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-c5-18xlarge-2021-01-30-rss-b.svg`
- `src/dashbls/depends/mimalloc/doc/bench-2021/bench-macmini-2021-01-30.svg`
- `src/dashbls/depends/mimalloc/doc/mimalloc-doc.h`
- `src/dashbls/depends/mimalloc/doc/mimalloc-doxygen.css`
- ... (913 more)

### `privacy_coinjoin`

- Smartie-only matches: `25`
- Diverged common matches: `0`

Smartie-only sample:
- `src/coinjoin/client.cpp`
- `src/coinjoin/client.h`
- `src/coinjoin/coinjoin.cpp`
- `src/coinjoin/coinjoin.h`
- `src/coinjoin/common.cpp`
- `src/coinjoin/common.h`
- `src/coinjoin/interfaces.cpp`
- `src/coinjoin/options.cpp`
- `src/coinjoin/options.h`
- `src/coinjoin/server.cpp`
- `src/coinjoin/server.h`
- `src/coinjoin/util.cpp`
- `src/coinjoin/util.h`
- `src/coinjoin/walletman.cpp`
- `src/coinjoin/walletman.h`
- `src/interfaces/coinjoin.h`
- `src/rpc/coinjoin.cpp`
- `src/test/coinjoin_basemanager_tests.cpp`
- `src/test/coinjoin_dstxmanager_tests.cpp`
- `src/test/coinjoin_inouts_tests.cpp`
- `src/test/coinjoin_queue_tests.cpp`
- `src/wallet/coinjoin.cpp`
- `src/wallet/coinjoin.h`
- `src/wallet/test/coinjoin_tests.cpp`
- `test/functional/rpc_coinjoin.py`

## Suggested Next Port Steps

1. Port shared utility/runtime drift first (`src/common`, `src/util`, `src/kernel`, RPC helpers) before consensus-heavy modules.
2. Keep Smartie feature islands isolated: YesPower PoW, Masternodes/LLMQ, CoinJoin.
3. Re-run this report after each batch and reduce `common_diff` in core directories while preserving feature islands.
