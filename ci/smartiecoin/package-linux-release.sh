#!/usr/bin/env bash
export LC_ALL=C
# Build a clean, release-shaped Linux x86_64 package from the CI build tree.
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
BUILD="$ROOT/build-ci/smartiecoin-linux64"
SRC="$BUILD/src"
OUT="$ROOT/release-artifacts-linux"
LOG="$OUT/relink.log"
CONFIG="$SRC/config/bitcoin-config.h"
mkdir -p "$OUT"

if [ ! -x "$SRC/smartiecoind" ] || [ ! -x "$SRC/smartiecoin-wallet" ] || [ ! -x "$SRC/qt/smartiecoin-qt" ]; then
  echo "Missing Linux GUI/wallet build outputs under $SRC" >&2
  exit 1
fi
if [ ! -f "$CONFIG" ]; then
  echo "Missing $CONFIG" >&2
  exit 1
fi
MAJOR=$(awk '$2 == "CLIENT_VERSION_MAJOR" {print $3}' "$CONFIG")
MINOR=$(awk '$2 == "CLIENT_VERSION_MINOR" {print $3}' "$CONFIG")
BUILDNUM=$(awk '$2 == "CLIENT_VERSION_BUILD" {print $3}' "$CONFIG")
if [ -z "$MAJOR" ] || [ -z "$MINOR" ] || [ -z "$BUILDNUM" ]; then
  echo "Could not read version macros from $CONFIG" >&2
  exit 1
fi
VERSION="$MAJOR.$MINOR.$BUILDNUM"

RUNTIME_PACKAGES_FILE="$ROOT/ci/smartiecoin/linux-runtime-packages.txt"
if [ ! -s "$RUNTIME_PACKAGES_FILE" ]; then
  echo "Missing Linux runtime package manifest: $RUNTIME_PACKAGES_FILE" >&2
  exit 1
fi
LINUX_RUNTIME_PACKAGES=()
while IFS=$'\t' read -r package soname extra; do
  if [[ -z "$package" || -z "$soname" || -n "$extra" || ! "$package" =~ ^[a-z0-9][a-z0-9.+-]*$ || ! "$soname" =~ ^lib[A-Za-z0-9][A-Za-z0-9._+-]*\.so(\.[0-9]+)*$ ]]; then
    echo "Invalid Linux runtime package/SONAME row: $package $soname $extra" >&2
    exit 1
  fi
  LINUX_RUNTIME_PACKAGES+=("$package")
done < "$RUNTIME_PACKAGES_FILE"
if [ "${#LINUX_RUNTIME_PACKAGES[@]}" -eq 0 ]; then
  echo "Linux runtime package manifest is empty: $RUNTIME_PACKAGES_FILE" >&2
  exit 1
fi
LINUX_RUNTIME_PACKAGES_LIST="${LINUX_RUNTIME_PACKAGES[*]}"
cp "$RUNTIME_PACKAGES_FILE" "$OUT/linux-runtime-packages.txt"

# GCC 13+ can split the release gate's wallet marker out of .rodata when it is
# emitted from a function-argument literal. Compile the actual wallet init TU at
# -O0, then relink every target so the final daemon and GUI share that object.
cd "$SRC"
rm -f wallet/libbitcoin_node_a-init.o
CMD=$(make -n wallet/libbitcoin_node_a-init.o 2>/dev/null | python3 -c '
import sys
text = sys.stdin.read().replace("\\\\\n", " ")
for line in text.splitlines():
    if "g++" in line and "init.cpp" in line and " -c " in line:
        print(line.strip())
        break
')
[ -n "$CMD" ] || { echo "Could not derive wallet init compile command" >&2; exit 1; }
CMD_O0=$(printf '%s\n' "$CMD" | sed -E 's/ -O([0-3sg])([[:space:]]|$)/ -O0\2/g')
if [[ "$CMD_O0" == "$CMD" ]]; then
  CMD_O0="${CMD/ -c / -O0 -c }"
fi
case " $CMD_O0 " in
  *" -O0 "*) ;;
  *) echo "Could not force wallet init optimization to -O0" >&2; exit 1 ;;
esac
echo "=== compile wallet/init.cpp with calibrated -O0 ==="
eval "$CMD_O0"
[ -s wallet/libbitcoin_node_a-init.o ] || { echo "wallet init object missing" >&2; exit 1; }

cd "$BUILD"
echo "=== relink all targets after init.o update ==="
make -j"$(nproc)" > "$LOG" 2>&1 || {
  printf 'Linux relink failed; full log: %s\n' "$LOG" >&2
  exit 1
}

PKG="$OUT/smartiecoin-$VERSION-linux64"
rm -rf "$PKG"
mkdir -p "$PKG/bin"
for name in smartiecoind smartiecoin-cli smartiecoin-tx smartiecoin-util smartiecoin-wallet; do
  [ -x "$SRC/$name" ] || { echo "Missing release binary $SRC/$name" >&2; exit 1; }
  cp "$SRC/$name" "$PKG/bin/$name"
done
[ -x "$SRC/qt/smartiecoin-qt" ] || { echo "Missing Qt release binary" >&2; exit 1; }
cp "$SRC/qt/smartiecoin-qt" "$PKG/bin/smartiecoin-qt"
strip "$PKG/bin/"*

cat > "$PKG/README.txt" <<EOF
Smartiecoin Core v$VERSION - Linux x86_64
Windows Qt wallet startup hotfix and clean release rebuild. No consensus changes.
Binaries: smartiecoind, smartiecoin-cli, smartiecoin-tx, smartiecoin-util,
smartiecoin-wallet, and smartiecoin-qt (GUI; Qt5 is statically linked).
The GUI requires system X11/XCB/XKB, fontconfig, and freetype runtime libraries.
Debian/Ubuntu: sudo apt-get install $LINUX_RUNTIME_PACKAGES_LIST
Other distributions must provide the equivalent shared-library SONAMEs.
Run the GUI:  ./bin/smartiecoin-qt
Run headless: ./bin/smartiecoind -daemon; then ./bin/smartiecoin-cli getblockcount
The Linux build contains its Sapling parameters; no external params directory is needed.
Website: https://smartiecoin.com
Issues: https://github.com/SmartiesCoin/Smartiecoin/issues
EOF

# Fail closed on the stripped binaries that will actually ship.
python3 "$ROOT/contrib/devtools/check-release-bdb.py" \
  --config-header "$CONFIG" \
  --wallet "$PKG/bin/smartiecoin-wallet" \
  --node "$PKG/bin/smartiecoind" \
  --node "$PKG/bin/smartiecoin-qt" \
  --scan-dir "$PKG/bin" \
  --runtime required \
  --report "$OUT/bdb-gate-linux-$VERSION.json"

# Isolated final-package regtest smoke: create wallet, mine, stop cleanly.
SMOKE=$(mktemp -d)
PORT=19394
cleanup() {
  "$PKG/bin/smartiecoin-cli" -regtest -datadir="$SMOKE" -rpcport="$PORT" \
    -rpcuser=smoke -rpcpassword=smoke stop >/dev/null 2>&1 || true
  rm -rf "$SMOKE"
}
trap cleanup EXIT
"$PKG/bin/smartiecoind" -regtest -datadir="$SMOKE" -server -listen=0 \
  -dnsseed=0 -connect=0 -upnp=0 -rpcport="$PORT" \
  -rpcuser=smoke -rpcpassword=smoke -daemon
CLI="$PKG/bin/smartiecoin-cli -regtest -datadir=$SMOKE -rpcport=$PORT -rpcuser=smoke -rpcpassword=smoke"
READY=0
for _ in $(seq 1 30); do
  if $CLI getblockcount >/dev/null 2>&1; then READY=1; break; fi
  sleep 2
done
[ "$READY" = 1 ] || { echo "Linux package RPC failed to start" >&2; exit 1; }
$CLI createwallet smoke >/dev/null
ADDR=$($CLI -rpcwallet=smoke getnewaddress)
$CLI generatetoaddress 1 "$ADDR" >/dev/null
HEIGHT=$($CLI getblockcount)
[ "$HEIGHT" = 1 ] || { echo "Expected regtest height 1, got $HEIGHT" >&2; exit 1; }
$CLI stop >/dev/null
for _ in $(seq 1 15); do
  if grep -q 'Shutdown: done' "$SMOKE/regtest/debug.log" 2>/dev/null; then break; fi
  sleep 1
done
grep -q 'Shutdown: done' "$SMOKE/regtest/debug.log"
grep -Eq 'BerkeleyDB version Berkeley DB ?4\.8\.30' "$SMOKE/regtest/debug.log"
trap - EXIT
rm -rf "$SMOKE"

echo "=== final package smoke/version ==="
"$PKG/bin/smartiecoind" --version | grep -F "v$VERSION"
strings "$PKG/bin/smartiecoin-qt" > "$OUT/qt-strings.txt"
grep -Fq "$VERSION" "$OUT/qt-strings.txt"

cd "$OUT"
tar -czf "smartiecoin-$VERSION-linux64.tar.gz" "smartiecoin-$VERSION-linux64"
tar -tzf "smartiecoin-$VERSION-linux64.tar.gz" >/dev/null
VERIFY=$(mktemp -d)
trap 'rm -rf "$VERIFY"' EXIT
tar -xzf "smartiecoin-$VERSION-linux64.tar.gz" -C "$VERIFY"
"$VERIFY/smartiecoin-$VERSION-linux64/bin/smartiecoind" --version | grep -F "v$VERSION"
echo "Linux release package, BDB gate, and regtest smoke passed: v$VERSION"
