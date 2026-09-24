# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Regresión estática de la política BDB del release Windows; no compila Windows.

Ejecutar: python3 test/lint/test_release_bdb_policy.py -v
Solo usa stdlib. Lee comandos y campos efectivos, no comentarios del workflow.
"""

from pathlib import Path
import re
import shlex
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/build-windows-msys2.yml"
BDB_SHA256 = "12edc0df75bf9abd7f82f821795bcee50f42cb2e5f76a6a281b85732798364ef"


def steps_from(text):
    """Extrae los pasos con nombre del subconjunto YAML usado por este workflow."""
    matches = list(re.finditer(r"^      - name: (.+)$", text, re.MULTILINE))
    return {
        match.group(1): text[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(text)]
        for index, match in enumerate(matches)
    }


def field_from(step, name):
    """Lee un escalar inline o bloque indentado; descarta comentarios completos."""
    match = re.search(rf"^( +){re.escape(name)}: (.*)$", step, re.MULTILINE)
    if not match:
        raise AssertionError(f"Falta el campo ejecutable {name}")
    if match.group(2) not in ("|", ">-"):
        return match.group(2)
    lines = []
    indent = len(match.group(1))
    for line in step[match.end():].splitlines():
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        if line.strip() and not line.lstrip().startswith("#"):
            lines.append(line[indent + 2:])
    return "\n".join(lines)


class ReleaseBDBPolicyTest(unittest.TestCase):
    def setUp(self):
        self.steps = steps_from(WORKFLOW.read_text(encoding="utf-8"))

    def run_script(self, name):
        self.assertIn(name, self.steps, f"Falta el paso obligatorio: {name}")
        return field_from(self.steps[name], "run")

    def test_pinned_static_bdb_is_built_and_selected(self):
        install = shlex.split(field_from(self.steps["Set up MSYS2 MinGW64"], "install"), comments=True)
        self.assertNotIn("mingw-w64-x86_64-db", install)
        build = self.run_script("Build pinned Berkeley DB 4.8.30")
        self.assertTrue(build.startswith("set -euo pipefail\n"))
        commands = build.replace("\\\n", "")
        self.assertIn('BDB_PREFIX="$PWD/bdb48"', commands)
        self.assertRegex(commands, r"curl --fail --location --retry 3 --output db-4\.8\.30\.NC\.tar\.gz\s+https://download\.oracle\.com/berkeley-db/db-4\.8\.30\.NC\.tar\.gz")
        self.assertIn(f"printf '%s  %s\\n' {BDB_SHA256} db-4.8.30.NC.tar.gz | sha256sum --check --strict", commands)
        self.assertLess(commands.index("sha256sum --check --strict"), commands.index("tar -xzf db-4.8.30.NC.tar.gz"))
        self.assertIn("patch -d db-4.8.30.NC -p1 < depends/patches/bdb/clang_cxx_11.patch", commands)
        self.assertIn("cp depends/config.guess depends/config.sub db-4.8.30.NC/dist/", commands)
        self.assertIn("cd db-4.8.30.NC/build_unix", commands)
        bdb_configure = next(line for line in commands.splitlines() if line.startswith("../dist/configure "))
        options = shlex.split(bdb_configure)
        for option in ("--host=x86_64-w64-mingw32", "--prefix=$BDB_PREFIX", "--enable-mingw", "--enable-cxx", "--enable-static", "--disable-shared", "--disable-replication", "CC=/mingw64/bin/gcc", "CXX=/mingw64/bin/g++", "CPPFLAGS=-DUNICODE -D_UNICODE"):
            self.assertIn(option, options)
        self.assertIn("make -j2 libdb_cxx-4.8.a libdb-4.8.a", commands)
        self.assertIn("make install_lib install_include", commands)
        for path in ("include/db.h", "include/db_cxx.h", "lib/libdb_cxx-4.8.a", "lib/libdb-4.8.a"):
            self.assertIn(f'test -s "$BDB_PREFIX/{path}"', commands)
        self.assertNotRegex(build, r"\|\|\s*(true|:)|set \+e")

        configure = self.run_script("configure").replace("\\\n", "")
        self.assertIn('BDB_PREFIX="$PWD/bdb48"', configure)
        invocation = next(line for line in configure.splitlines() if line.startswith("./configure "))
        options = shlex.split(invocation)
        for option in ("--enable-wallet", "--with-bdb", "--enable-util-wallet", "BDB_CFLAGS=-I$BDB_PREFIX/include", "BDB_LIBS=$BDB_PREFIX/lib/libdb_cxx-4.8.a $BDB_PREFIX/lib/libdb-4.8.a"):
            self.assertIn(option, options)
        self.assertNotIn("--with-incompatible-bdb", options)
        self.assertNotIn("--disable-wallet", options)
        self.assertNotIn("--without-bdb", options)
        names = list(self.steps)
        self.assertLess(names.index("Build pinned Berkeley DB 4.8.30"), names.index("configure"))

    def test_binary_gate_fails_closed_before_upload(self):
        gate = self.run_script("Verify release BDB policy")
        self.assertTrue(gate.startswith("set -euo pipefail\n"))
        self.assertIn("export LC_ALL=C", gate)
        self.assertIn("grep -qx '#define ENABLE_WALLET 1' src/config/bitcoin-config.h", gate)
        self.assertIn("grep -qx '#define USE_BDB 1' src/config/bitcoin-config.h", gate)
        self.assertIn("artifact_dir=artifacts/smartiecoin-windows-msys2", gate)
        self.assertIn("check_dir=bdb-release-check", gate)
        self.assertIn('mkdir -p "$check_dir"', gate)
        self.assertRegex(gate, r'for name in smartiecoind smartiecoin-cli smartiecoin-wallet smartiecoin-qt; do\n  binary="\$artifact_dir/\$name.exe"\n  test -s "\$binary"\n  objdump -p "\$binary" > "\$check_dir/\$name.imports"')
        self.assertIn('grep -q "DLL Name:" "$check_dir/$name.imports"', gate)
        self.assertIn("if grep -iE 'DLL Name:[[:space:]]*(lib)?(db[_.0-9-]|berkeley)' \"$check_dir/$name.imports\"; then", gate)
        self.assertRegex(gate, r'echo "[^"\n]+" >&2\n    exit 1\n  else\n    status=\$\?\n    test "\$status" -eq 1\n  fi')
        self.assertIn('for name in smartiecoind smartiecoin-wallet smartiecoin-qt; do', gate)
        self.assertIn('strings "$check_dir/$name.rdata" > "$check_dir/$name.strings"', gate)
        self.assertIn('versions=$(grep -oE \'Berkeley DB [0-9]+\\.[0-9]+\\.[0-9]+\' "$check_dir/$name.strings" | sort -u)', gate)
        self.assertIn('test "$versions" = "Berkeley DB 4.8.30"', gate)
        self.assertRegex(gate, r'for name in smartiecoind smartiecoin-qt; do\n  grep -Fxq -- \'-wallet=<path>\' "\$check_dir/\$name.strings"\ndone')
        self.assertIn('"$artifact_dir/smartiecoin-wallet.exe" -help > "$check_dir/wallet-help.txt"', gate)
        self.assertIn('grep -Fq "Get wallet info" "$check_dir/wallet-help.txt"', gate)
        self.assertNotRegex(gate, r"\|\|\s*(true|:)|set \+e|\bhead\b|exit 0")
        for name in ("Build pinned Berkeley DB 4.8.30", "configure", "collect binaries", "Verify release BDB policy", "Upload artifact"):
            self.assertNotRegex(self.steps[name], r"(?m)^        (?:if|continue-on-error):")
        names = list(self.steps)
        self.assertLess(names.index("make"), names.index("collect binaries"))
        self.assertLess(names.index("collect binaries"), names.index("Verify release BDB policy"))
        self.assertLess(names.index("Verify release BDB policy"), names.index("Upload artifact"))

    def test_recovery_resources_are_excluded_and_wallet_is_exercised(self):
        build = self.run_script("Build pinned Berkeley DB 4.8.30")
        self.assertIn("chmod u+w db-4.8.30.NC/dist/config.guess db-4.8.30.NC/dist/config.sub", build)
        self.assertLess(build.index("chmod u+w"), build.index("cp depends/config.guess"))
        gate = self.run_script("Verify release BDB policy")
        # Recovery executables/DLLs in .rsrc legitimately contain BDB 6.2 strings.
        self.assertIn('objcopy -O binary --only-section=.rdata "$artifact_dir/$name.exe" "$check_dir/$name.rdata"', gate)
        self.assertIn('test -s "$check_dir/$name.rdata"', gate)
        self.assertIn('strings "$check_dir/$name.rdata" > "$check_dir/$name.strings"', gate)
        self.assertNotIn('strings "$artifact_dir/$name.exe"', gate)
        self.assertIn('test ! -e "$check_dir/wallet"', gate)
        self.assertIn('wallet_path=$(cygpath -m "$PWD/$check_dir/wallet")', gate)
        self.assertIn('data_path=$(cygpath -m "$PWD/$check_dir/data")', gate)
        for command in ("create", "info"):
            self.assertIn(f'"$artifact_dir/smartiecoin-wallet.exe" "-datadir=$data_path" "-wallet=$wallet_path" {command} > "$check_dir/wallet-{command}.txt"', gate)
            self.assertIn(f'grep -qx "Format: bdb" "$check_dir/wallet-{command}.txt"', gate)
        self.assertIn('assert int.from_bytes(header[16:20], "little") == 9', gate)
        self.assertIn('"$check_dir/wallet/wallet.dat"', gate)

    def test_wallet_tool_is_packaged_and_policy_is_run(self):
        collect = self.run_script("collect binaries")
        for binary in ("smartiecoind", "smartiecoin-cli", "smartiecoin-wallet", "qt/smartiecoin-qt"):
            self.assertRegex(collect, rf"(?m)^cp src/{re.escape(binary)}\.exe +artifacts/smartiecoin-windows-msys2/$")
        self.assertTrue(collect.startswith("set -euo pipefail\n"))
        self.assertIn("strip artifacts/smartiecoin-windows-msys2/smartiecoin-wallet.exe", collect)
        upload = self.steps["Upload artifact"]
        self.assertEqual(field_from(upload, "path"), "artifacts/smartiecoin-windows-msys2/")
        self.assertEqual(field_from(upload, "if-no-files-found"), "error")
        lint = self.run_script("Test release BDB policy")
        self.assertEqual(lint, "python3 test/lint/test_release_bdb_policy.py -v")
        self.assertLess(list(self.steps).index("Test release BDB policy"), list(self.steps).index("Build pinned Berkeley DB 4.8.30"))


if __name__ == "__main__":
    unittest.main()
