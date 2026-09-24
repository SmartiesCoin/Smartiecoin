#!/usr/bin/env bash
# Compare build variants of the miner on this machine (no node needed). Every variant must print the same
# selftest digest: that proves it computes exactly the consensus hash. Stop other miners first.
#
#   contrib/smt-miner/bench.sh [--threads N] [--seconds S]
#
# The x86-only variants (SSE2 baseline, AVX2, huge pages) are skipped on other architectures; there the two-way
# interleaved kernel (smt-yp2.c) is not built either, so only the reference kernel is measured.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
yp="$here/../../src/crypto/yespower"
cpus() { nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2; }
THREADS="$(( $(cpus) - 2 ))" SECS=10
while [ $# -gt 0 ]; do
    case "$1" in
        --threads) THREADS="${2:?}"; shift ;;
        --seconds) SECS="${2:?}"; shift ;;
        -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
LIBS="-lcurl -ljansson -lcrypto -lm"
INC=""
LIB=""
for pfx in "${HOMEBREW_PREFIX:-}" /opt/homebrew /usr/local; do
    [ -d "$pfx" ] || continue
    [ -f "$pfx/include/jansson.h" ] && { INC="$INC -I$pfx/include"; LIB="$LIB -L$pfx/lib"; }
    for ssl in "$pfx/opt/openssl" "$pfx/opt/openssl@3" "$pfx/opt/openssl@1.1"; do
        [ -f "$ssl/include/openssl/sha.h" ] && { INC="$INC -I$ssl/include"; LIB="$LIB -L$ssl/lib"; break; }
    done
done

case "$(uname -m)" in
    x86_64|amd64|i?86) YP2=1 ;;   # 1 = the two-way interleaved kernel can be built (x86 with SSE2)
    *)                 YP2=0 ;;
esac
YP2FLAG=""
SRCS=("$here/smt-miner.c")
if [ "$YP2" = 1 ]; then
    SRCS+=("$here/smt-yp2.c")
else
    YP2FLAG="-DSMT_NO_YP2"
fi

build() {   # build <name> <sed script for yespower-platform.c or ""> <cflags...>
    local name="$1" patch="$2"; shift 2
    cp -r "$yp" "$work/y_$name"
    if [ -n "$patch" ]; then
        sed "$patch" "$work/y_$name/yespower-platform.c" > "$work/y_$name/p.c" &&
            mv "$work/y_$name/p.c" "$work/y_$name/yespower-platform.c"
    fi
    # shellcheck disable=SC2086
    gcc "$@" $YP2FLAG $INC -pthread "-I$work/y_$name" "-I$here" -o "$work/m_$name" "${SRCS[@]}" "$work/y_$name/yespower.c" $LIBS $LIB 2>/dev/null ||
        echo "  (variant $name does not build with this compiler/CPU)"
}
run() {
    [ -x "$work/m_$1" ] || return
    local w
    for w in 1 $([ "$YP2" = 1 ] && echo 2); do   # 1 = reference kernel, 2 = two-way interleaved kernel
        printf '%-16s' "$1 ways=$w"
        "$work/m_$1" --bench "$SECS" --threads "$THREADS" --ways "$w" 2>&1 | sed -n '1p;$p' |
            sed -E 's/threads=[0-9]+ (ways=[0-9]+ )?seconds=[0-9.]+ //; s/selftest digest: /digest=/' | tr '\n' ' '
        echo
    done
}

echo "threads=$THREADS, ${SECS}s per variant, host=$(uname -m); Linux huge pages reserved: $(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 'n/a')"
if [ "$YP2" = 1 ]; then
    build native    ""  -O3 -march=native
    build sse2      ""  -O3 -march=x86-64 -msse2
    build avx2      ""  -O3 -march=x86-64-v3
    if [ "$(uname -s)" = Linux ]; then
        build hugepages 's/(12 \* 1024 \* 1024)/(1)/' -O3 -march=native
    fi
    for v in native sse2 avx2 hugepages; do run "$v"; done
else
    build native    ""  -O3 -march=native
    build o2        ""  -O2
    for v in native o2; do run "$v"; done
fi
echo "All digests must be identical. Speed differences under about 2% are measurement noise."
