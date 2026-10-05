#!/usr/bin/env python3
"""Compile actual sources and compare serialization to the executed Boost baseline.

Requires C++20 and the project's development headers. Never rewrites goldens.
Use --build-dir outside the source tree; CPPFLAGS supplies dependency includes.
This is a serialization/KeyIO gate, not a wallet DB or release-build approval.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--rust-lib", type=Path, help="Project librustzcash.a for the real Pedersen tree/witness gate")
    args = parser.parse_args()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    src = here.parents[1] / "src"
    expected = json.loads((here / "boost_baseline.json").read_text())
    walletdb = (src / "wallet/walletdb.cpp").read_text()
    for symbol, tag in [("SAP_KEY", "sapzkey"), ("SAP_ADDR", "sapzaddr"), ("SAP_KEY_CRIPTED", "csapzkey")]:
        assert f'const std::string {symbol}{{"{tag}"}};' in walletdb
    common = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++20", "-O0", "-UNDEBUG"]
    common += shlex.split(os.environ.get("CPPFLAGS", ""))
    common += ["-I" + str(src / path) for path in ["", "secp256k1/include", "univalue/include", "rust/include"]]
    link = ["-ffunction-sections", "-fdata-sections"]
    link += ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]
    receipt = {}

    def run(name, command):
        result = subprocess.run(command, text=True, capture_output=True, timeout=240)
        receipt[name] = {"command": command, "exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
        (args.build_dir / "results.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if result.returncode:
            print(result.stdout + result.stderr, file=sys.stderr)
            raise RuntimeError(f"{name} failed ({result.returncode})")
        return result.stdout

    run("std-types", common + ["-fsyntax-only", str(here / "std_types.cpp")])
    for name in ["wire", "keyio", "sapling_serialization"] + (["merkle"] if args.rust_lib else []):
        sources = [here / (name + ".cpp"), src / "support/cleanse.cpp", src / "uint256.cpp"]
        if name == "keyio":
            sources += [src / p for p in ["sapling/key_io_sapling.cpp", "sapling/address.cpp", "sapling/zip32.cpp", "bech32.cpp"]]
        if name == "sapling_serialization":
            sources += [src / path for path in ["sapling/incrementalmerkletree.cpp", "primitives/transaction.cpp", "crypto/sha256.cpp"]]
        extra_link = []
        if name == "merkle":
            sources += [src / "sapling/incrementalmerkletree.cpp"]
            extra_link = [str(args.rust_lib)]
            if sys.platform == "darwin":
                extra_link += ["-framework", "Security", "-framework", "CoreFoundation"]
            else:
                extra_link += ["-ldl", "-lpthread", "-lm"]
        exe = args.build_dir / name
        run(name + "-compile", common + link + list(map(str, sources)) + extra_link + ["-o", str(exe)])
        output = run(name, [str(exe)])
        assert output == expected[name], f"{name}: pre-migration golden bytes changed"
        print(f"PASS {name}: byte-identical to executed Boost baseline")
    if not args.rust_lib:
        print("NOT RUN merkle: pass --rust-lib to exercise real Pedersen hashing")


if __name__ == "__main__":
    main()
