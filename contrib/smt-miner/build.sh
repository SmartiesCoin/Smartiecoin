#!/usr/bin/env bash
# Build the miner: smt-miner (native, recommended) and libsmt.so (used by smt_miner.py and the reference pool).
# Needs: gcc/clang, libcurl, jansson and OpenSSL development files, for example
#   Debian/Ubuntu    apt install gcc libcurl4-openssl-dev libjansson-dev libssl-dev
#   macOS (brew)     xcode-select --install; brew install jansson openssl@3     (libcurl comes with the SDK)
#   MSYS2 (Windows)  pacman -S mingw-w64-ucrt-x86_64-{gcc,curl,jansson,openssl}
#
# Environment options:
#   SMT_CFLAGS      compiler flags (default: "-O3 -march=native"; use "-O3 -march=x86-64-v2" for a portable binary)
#   SMT_HUGEPAGES=1 build smt-miner so that its Yespower memory is allocated with 2 MB huge pages when the system
#                   has some reserved (Linux only; see setup-hugepages.sh). The node's yespower sources are NOT
#                   modified: the change is applied to a private temporary copy, and it only affects where memory
#                   comes from, never the hash results (check with: smt-miner --selftest).
#
# smt-miner includes an optional two-way interleaved Yespower kernel (smt-yp2.c, about +35% hashrate). That kernel
# is x86/SSE2 only by design: on any other architecture (for example Apple Silicon) the miner is built with
# -DSMT_NO_YP2 and hashes one block at a time with the reference kernel. On x86 the two-way kernel is tried first
# and the same fallback applies when it does not compile. Either way the miner is fully functional, and because it
# calls the node's own yespower_hash() it can never disagree with consensus about the PoW function.
# shellcheck disable=SC2086  # CFLAGS and LIBS are intentionally word-split
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
yp="$here/../../src/crypto/yespower"
CF="${SMT_CFLAGS:--O3 -march=native}"
LIBS="-lcurl -ljansson -lcrypto -lm"

# Windows (MSYS2/MinGW) needs the WinSock import library for the sockets the stratum client uses.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) LIBS="$LIBS -lws2_32" ;;
esac

# Extra include/library search paths. Homebrew (macOS) keeps jansson outside the default path and OpenSSL is a
# keg-only formula, so its own prefix has to be added. CPPFLAGS/LDFLAGS from the environment are always honoured.
INC="${CPPFLAGS:-}"
LIB="${LDFLAGS:-}"
for pfx in "${HOMEBREW_PREFIX:-}" /opt/homebrew /usr/local; do
    [ -d "$pfx" ] || continue
    if [ -f "$pfx/include/jansson.h" ]; then
        INC="$INC -I$pfx/include"
        LIB="$LIB -L$pfx/lib"
    fi
    for ssl in "$pfx/opt/openssl" "$pfx/opt/openssl@3" "$pfx/opt/openssl@1.1"; do
        if [ -f "$ssl/include/openssl/sha.h" ]; then
            INC="$INC -I$ssl/include"
            LIB="$LIB -L$ssl/lib"
            break
        fi
    done
done

# Shared library for the Python miner and the reference pool (optional: needs an OpenMP-enabled compiler).
solib="$here/libsmt.so"
solog="${TMPDIR:-/tmp}/libsmt-build.log"
if gcc $CF $INC -fopenmp -shared -fPIC -I"$yp" -o "$solib" "$here/scan.c" "$yp/yespower.c" 2>"$solog"; then
    echo "built $solib"
else
    echo "libsmt.so not built: needs an OpenMP-enabled compiler (on macOS: brew install libomp, then re-run); see $solog"
fi

# The two-way kernel is x86/SSE2 only, so only x86 hosts try to build it.
case "$(uname -m)" in
    x86_64|amd64|i?86) try_yp2=1 ;;
    *)                 try_yp2=0 ;;
esac

ypb="$yp"
if [ "${SMT_HUGEPAGES:-0}" = 1 ]; then
    if [ "$(uname -s)" != Linux ]; then
        echo "SMT_HUGEPAGES=1 is Linux only (2 MB huge pages); see setup-hugepages.sh" >&2
        exit 1
    fi
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    cp -r "$yp"/. "$tmp"/
    # the upstream allocator only asks for huge pages above 12 MB; Smartiecoin's regions are about 256 KB
    sed 's/(12 \* 1024 \* 1024)/(1)/' "$tmp/yespower-platform.c" > "$tmp/yespower-platform.c.new"
    mv "$tmp/yespower-platform.c.new" "$tmp/yespower-platform.c"
    grep -q 'define HUGEPAGE_THRESHOLD[[:space:]]*(1)' "$tmp/yespower-platform.c" ||
        { echo "error: could not patch the huge page threshold (upstream file changed?)" >&2; exit 1; }
    ypb="$tmp"
    echo "huge pages enabled in this build"
fi

log="${TMPDIR:-/tmp}/smt-miner-build.log"
bin="$here/smt-miner"
if [ "$try_yp2" = 1 ]; then
    srcs=("$here/smt-miner.c" "$here/smt-yp2.c")
    yp2flag=""
    note="with the two-way kernel"
else
    srcs=("$here/smt-miner.c")
    yp2flag="-DSMT_NO_YP2"
    note="reference kernel only: the two-way kernel is x86/SSE2 only, this host is $(uname -m)"
fi

if gcc $CF $INC -pthread $yp2flag -I"$ypb" -I"$here" -o "$bin" "${srcs[@]}" "$ypb/yespower.c" $LIBS $LIB 2>"$log"; then
    :
elif [ "$try_yp2" = 1 ] &&
     gcc $CF $INC -pthread -DSMT_NO_YP2 -I"$ypb" -I"$here" -o "$bin" "$here/smt-miner.c" "$ypb/yespower.c" $LIBS $LIB 2>>"$log"; then
    note="reference kernel only: the two-way kernel did not compile, see $log"
else
    echo "native miner not built (install libcurl, jansson and OpenSSL development files); see $log" >&2
    exit 1
fi
echo "built $bin ($note)"
