# Bitcoin v30.2 Phase-2 Runtime Worklist

- Generated: `2026-02-23T05:02:14Z`
- Source report: `C:\Dev\Smartiecoin\doc\porting\bitcoin-v30.2-gap-report.json`

## Scope

This worklist focuses on runtime/core alignment and excludes Smartie feature islands (YesPower, Masternodes/LLMQ, CoinJoin).

## Summary

| Metric | Count |
| --- | ---: |
| Runtime common-diff candidates | 165 |
| Runtime bitcoin-only candidates | 171 |

## Runtime Common-Diff by Subdirectory

| Subdirectory | Files |
| --- | ---: |
| `src/util` | 47 |
| `src/node` | 29 |
| `src/rpc` | 28 |
| `src/script` | 17 |
| `src/consensus` | 10 |
| `src/ipc` | 10 |
| `src/policy` | 10 |
| `src/interfaces` | 8 |
| `src/common` | 4 |
| `src/kernel` | 2 |

## Runtime Bitcoin-Only by Subdirectory

| Subdirectory | Files |
| --- | ---: |
| `src/ipc` | 70 |
| `src/node` | 34 |
| `src/util` | 28 |
| `src/kernel` | 25 |
| `src/policy` | 8 |
| `src/script` | 4 |
| `src/interfaces` | 2 |

## Candidate Files: Common-Diff

- `src/common/bloom.cpp`
- `src/common/bloom.h`
- `src/common/run_command.cpp`
- `src/common/run_command.h`
- `src/consensus/amount.h`
- `src/consensus/consensus.h`
- `src/consensus/merkle.cpp`
- `src/consensus/merkle.h`
- `src/consensus/params.h`
- `src/consensus/tx_check.cpp`
- `src/consensus/tx_check.h`
- `src/consensus/tx_verify.cpp`
- `src/consensus/tx_verify.h`
- `src/consensus/validation.h`
- `src/interfaces/README.md`
- `src/interfaces/chain.h`
- `src/interfaces/echo.h`
- `src/interfaces/handler.h`
- `src/interfaces/init.h`
- `src/interfaces/ipc.h`
- `src/interfaces/node.h`
- `src/interfaces/wallet.h`
- `src/ipc/capnp/context.h`
- `src/ipc/capnp/init-types.h`
- `src/ipc/capnp/protocol.cpp`
- `src/ipc/capnp/protocol.h`
- `src/ipc/context.h`
- `src/ipc/exception.h`
- `src/ipc/interfaces.cpp`
- `src/ipc/process.cpp`
- `src/ipc/process.h`
- `src/ipc/protocol.h`
- `src/kernel/coinstats.cpp`
- `src/kernel/coinstats.h`
- `src/node/README.md`
- `src/node/blockstorage.cpp`
- `src/node/blockstorage.h`
- `src/node/caches.cpp`
- `src/node/caches.h`
- `src/node/chainstate.cpp`
- `src/node/chainstate.h`
- `src/node/coin.cpp`
- `src/node/coin.h`
- `src/node/connection_types.cpp`
- `src/node/connection_types.h`
- `src/node/context.cpp`
- `src/node/context.h`
- `src/node/eviction.cpp`
- `src/node/eviction.h`
- `src/node/interface_ui.cpp`
- `src/node/interface_ui.h`
- `src/node/interfaces.cpp`
- `src/node/miner.cpp`
- `src/node/miner.h`
- `src/node/minisketchwrapper.cpp`
- `src/node/minisketchwrapper.h`
- `src/node/psbt.cpp`
- `src/node/psbt.h`
- `src/node/transaction.cpp`
- `src/node/transaction.h`
- `src/node/txreconciliation.cpp`
- `src/node/txreconciliation.h`
- `src/node/utxo_snapshot.h`
- `src/policy/feerate.cpp`
- `src/policy/feerate.h`
- `src/policy/fees.cpp`
- `src/policy/fees.h`
- `src/policy/packages.cpp`
- `src/policy/packages.h`
- `src/policy/policy.cpp`
- `src/policy/policy.h`
- `src/policy/settings.cpp`
- `src/policy/settings.h`
- `src/rpc/blockchain.cpp`
- `src/rpc/blockchain.h`
- `src/rpc/client.cpp`
- `src/rpc/client.h`
- `src/rpc/external_signer.cpp`
- `src/rpc/fees.cpp`
- `src/rpc/mempool.cpp`
- `src/rpc/mempool.h`
- `src/rpc/mining.cpp`
- `src/rpc/mining.h`
- `src/rpc/net.cpp`
- `src/rpc/node.cpp`
- `src/rpc/output_script.cpp`
- `src/rpc/protocol.h`
- `src/rpc/rawtransaction.cpp`
- `src/rpc/rawtransaction_util.cpp`
- `src/rpc/rawtransaction_util.h`
- `src/rpc/register.h`
- `src/rpc/request.cpp`
- `src/rpc/request.h`
- `src/rpc/server.cpp`
- `src/rpc/server.h`
- `src/rpc/server_util.cpp`
- `src/rpc/server_util.h`
- `src/rpc/signmessage.cpp`
- `src/rpc/txoutproof.cpp`
- `src/rpc/util.cpp`
- `src/rpc/util.h`
- `src/script/descriptor.cpp`
- `src/script/descriptor.h`
- `src/script/interpreter.cpp`
- `src/script/interpreter.h`
- `src/script/keyorigin.h`
- `src/script/miniscript.cpp`
- `src/script/miniscript.h`
- `src/script/script.cpp`
- `src/script/script.h`
- `src/script/script_error.cpp`
- `src/script/script_error.h`
- `src/script/sigcache.cpp`
- `src/script/sigcache.h`
- `src/script/sign.cpp`
- `src/script/sign.h`
- `src/script/signingprovider.cpp`
- `src/script/signingprovider.h`
- `src/util/asmap.cpp`
- `src/util/asmap.h`
- `src/util/bip32.cpp`
- `src/util/bip32.h`
- `src/util/bytevectorhash.cpp`
- `src/util/bytevectorhash.h`
- `src/util/check.cpp`
- `src/util/check.h`
- `src/util/epochguard.h`
- `src/util/fastrange.h`
- `src/util/golombrice.h`
- `src/util/hash_type.h`
- `src/util/hasher.cpp`
- `src/util/hasher.h`
- `src/util/macros.h`
- `src/util/moneystr.cpp`
- `src/util/moneystr.h`
- `src/util/overflow.h`
- `src/util/overloaded.h`
- `src/util/readwritefile.cpp`
- `src/util/readwritefile.h`
- `src/util/result.h`
- `src/util/serfloat.cpp`
- `src/util/serfloat.h`
- `src/util/sock.cpp`
- `src/util/sock.h`
- `src/util/strencodings.cpp`
- `src/util/strencodings.h`
- `src/util/string.cpp`
- `src/util/string.h`
- `src/util/syserror.cpp`
- `src/util/syserror.h`
- `src/util/thread.cpp`
- `src/util/thread.h`
- `src/util/threadinterrupt.cpp`
- `src/util/threadinterrupt.h`
- `src/util/threadnames.cpp`
- `src/util/threadnames.h`
- `src/util/time.cpp`
- `src/util/time.h`
- `src/util/tokenpipe.cpp`
- `src/util/tokenpipe.h`
- `src/util/trace.h`
- `src/util/translation.h`
- `src/util/types.h`
- `src/util/ui_change_type.h`
- `src/util/vector.h`

## Candidate Files: Bitcoin-Only

- `src/interfaces/mining.h`
- `src/interfaces/types.h`
- `src/ipc/.clang-tidy.in`
- `src/ipc/CMakeLists.txt`
- `src/ipc/capnp/common-types.h`
- `src/ipc/capnp/echo-types.h`
- `src/ipc/capnp/mining-types.h`
- `src/ipc/capnp/mining.cpp`
- `src/ipc/libmultiprocess/.gitignore`
- `src/ipc/libmultiprocess/CMakeLists.txt`
- `src/ipc/libmultiprocess/README.md`
- `src/ipc/libmultiprocess/ci/README.md`
- `src/ipc/libmultiprocess/ci/configs/default.bash`
- `src/ipc/libmultiprocess/ci/configs/freebsd.bash`
- `src/ipc/libmultiprocess/ci/configs/gnu32.bash`
- `src/ipc/libmultiprocess/ci/configs/llvm.bash`
- `src/ipc/libmultiprocess/ci/configs/macos.bash`
- `src/ipc/libmultiprocess/ci/configs/olddeps.bash`
- `src/ipc/libmultiprocess/ci/configs/openbsd.bash`
- `src/ipc/libmultiprocess/ci/configs/sanitize.bash`
- `src/ipc/libmultiprocess/ci/scripts/ci.sh`
- `src/ipc/libmultiprocess/ci/scripts/run.sh`
- `src/ipc/libmultiprocess/cmake/Config.cmake.in`
- `src/ipc/libmultiprocess/cmake/TargetCapnpSources.cmake`
- `src/ipc/libmultiprocess/cmake/compat_config.cmake`
- `src/ipc/libmultiprocess/cmake/compat_find.cmake`
- `src/ipc/libmultiprocess/cmake/pthread_checks.cmake`
- `src/ipc/libmultiprocess/doc/design.md`
- `src/ipc/libmultiprocess/doc/install.md`
- `src/ipc/libmultiprocess/doc/usage.md`
- `src/ipc/libmultiprocess/example/CMakeLists.txt`
- `src/ipc/libmultiprocess/example/calculator.cpp`
- `src/ipc/libmultiprocess/example/calculator.h`
- `src/ipc/libmultiprocess/example/example.cpp`
- `src/ipc/libmultiprocess/example/init.h`
- `src/ipc/libmultiprocess/example/printer.cpp`
- `src/ipc/libmultiprocess/example/printer.h`
- `src/ipc/libmultiprocess/example/types.h`
- `src/ipc/libmultiprocess/include/mp/config.h.in`
- `src/ipc/libmultiprocess/include/mp/proxy-io.h`
- `src/ipc/libmultiprocess/include/mp/proxy-types.h`
- `src/ipc/libmultiprocess/include/mp/proxy.h`
- `src/ipc/libmultiprocess/include/mp/type-char.h`
- `src/ipc/libmultiprocess/include/mp/type-chrono.h`
- `src/ipc/libmultiprocess/include/mp/type-context.h`
- `src/ipc/libmultiprocess/include/mp/type-data.h`
- `src/ipc/libmultiprocess/include/mp/type-decay.h`
- `src/ipc/libmultiprocess/include/mp/type-exception.h`
- `src/ipc/libmultiprocess/include/mp/type-function.h`
- `src/ipc/libmultiprocess/include/mp/type-interface.h`
- `src/ipc/libmultiprocess/include/mp/type-map.h`
- `src/ipc/libmultiprocess/include/mp/type-message.h`
- `src/ipc/libmultiprocess/include/mp/type-number.h`
- `src/ipc/libmultiprocess/include/mp/type-optional.h`
- `src/ipc/libmultiprocess/include/mp/type-pair.h`
- `src/ipc/libmultiprocess/include/mp/type-pointer.h`
- `src/ipc/libmultiprocess/include/mp/type-set.h`
- `src/ipc/libmultiprocess/include/mp/type-string.h`
- `src/ipc/libmultiprocess/include/mp/type-struct.h`
- `src/ipc/libmultiprocess/include/mp/type-threadmap.h`
- `src/ipc/libmultiprocess/include/mp/type-tuple.h`
- `src/ipc/libmultiprocess/include/mp/type-vector.h`
- `src/ipc/libmultiprocess/include/mp/type-void.h`
- `src/ipc/libmultiprocess/include/mp/util.h`
- `src/ipc/libmultiprocess/pkgconfig/libmultiprocess.pc.in`
- `src/ipc/libmultiprocess/src/mp/gen.cpp`
- `src/ipc/libmultiprocess/src/mp/proxy.cpp`
- `src/ipc/libmultiprocess/src/mp/util.cpp`
- `src/ipc/libmultiprocess/test/CMakeLists.txt`
- `src/ipc/libmultiprocess/test/mp/test/foo-types.h`
- `src/ipc/libmultiprocess/test/mp/test/foo.h`
- `src/ipc/libmultiprocess/test/mp/test/test.cpp`
- `src/kernel/CMakeLists.txt`
- `src/kernel/bitcoinkernel.cpp`
- `src/kernel/blockmanager_opts.h`
- `src/kernel/caches.h`
- `src/kernel/chain.cpp`
- `src/kernel/chain.h`
- `src/kernel/chainparams.cpp`
- `src/kernel/chainparams.h`
- `src/kernel/chainstatemanager_opts.h`
- `src/kernel/checks.cpp`
- `src/kernel/checks.h`
- `src/kernel/context.cpp`
- `src/kernel/context.h`
- `src/kernel/cs_main.cpp`
- `src/kernel/cs_main.h`
- `src/kernel/disconnected_transactions.cpp`
- `src/kernel/disconnected_transactions.h`
- `src/kernel/mempool_entry.h`
- `src/kernel/mempool_limits.h`
- `src/kernel/mempool_options.h`
- `src/kernel/mempool_removal_reason.cpp`
- `src/kernel/mempool_removal_reason.h`
- `src/kernel/messagestartchars.h`
- `src/kernel/notifications_interface.h`
- `src/kernel/warning.h`
- `src/node/abort.cpp`
- `src/node/abort.h`
- `src/node/blockmanager_args.cpp`
- `src/node/blockmanager_args.h`
- `src/node/chainstatemanager_args.cpp`
- `src/node/chainstatemanager_args.h`
- `src/node/coins_view_args.cpp`
- `src/node/coins_view_args.h`
- `src/node/database_args.cpp`
- `src/node/database_args.h`
- `src/node/kernel_notifications.cpp`
- `src/node/kernel_notifications.h`
- `src/node/mempool_args.cpp`
- `src/node/mempool_args.h`
- `src/node/mempool_persist.cpp`
- `src/node/mempool_persist.h`
- `src/node/mempool_persist_args.cpp`
- `src/node/mempool_persist_args.h`
- `src/node/mini_miner.cpp`
- `src/node/mini_miner.h`
- `src/node/peerman_args.cpp`
- `src/node/peerman_args.h`
- `src/node/protocol_version.h`
- `src/node/timeoffsets.cpp`
- `src/node/timeoffsets.h`
- `src/node/txdownloadman.h`
- `src/node/txdownloadman_impl.cpp`
- `src/node/txdownloadman_impl.h`
- `src/node/txorphanage.cpp`
- `src/node/txorphanage.h`
- `src/node/types.h`
- `src/node/utxo_snapshot.cpp`
- `src/node/warnings.cpp`
- `src/node/warnings.h`
- `src/policy/ephemeral_policy.cpp`
- `src/policy/ephemeral_policy.h`
- `src/policy/fees_args.cpp`
- `src/policy/fees_args.h`
- `src/policy/rbf.cpp`
- `src/policy/rbf.h`
- `src/policy/truc_policy.cpp`
- `src/policy/truc_policy.h`
- `src/script/parsing.cpp`
- `src/script/parsing.h`
- `src/script/solver.cpp`
- `src/script/solver.h`
- `src/util/CMakeLists.txt`
- `src/util/any.h`
- `src/util/batchpriority.cpp`
- `src/util/batchpriority.h`
- `src/util/bitdeque.h`
- `src/util/bitset.h`
- `src/util/byte_units.h`
- `src/util/chaintype.cpp`
- `src/util/chaintype.h`
- `src/util/exception.cpp`
- `src/util/exception.h`
- `src/util/exec.cpp`
- `src/util/exec.h`
- `src/util/feefrac.cpp`
- `src/util/feefrac.h`
- `src/util/fs.cpp`
- `src/util/fs.h`
- `src/util/fs_helpers.cpp`
- `src/util/fs_helpers.h`
- `src/util/insert.h`
- `src/util/obfuscation.h`
- `src/util/rbf.cpp`
- `src/util/rbf.h`
- `src/util/signalinterrupt.cpp`
- `src/util/signalinterrupt.h`
- `src/util/subprocess.h`
- `src/util/task_runner.h`
- `src/util/vecdeque.h`
