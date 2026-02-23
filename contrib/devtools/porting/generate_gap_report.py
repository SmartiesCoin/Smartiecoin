#!/usr/bin/env python3
"""
Generate a structured gap report between Smartiecoin and upstream Bitcoin Core.
"""

from __future__ import annotations

import argparse
import collections
import datetime
import hashlib
import json
import os
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


RELEVANT_TOP_LEVEL_DIRS = {
    "src",
    "test",
    "doc",
    "contrib",
    "depends",
    "build-aux",
    "share",
    "cmake",
}

IGNORED_DIR_NAMES = {
    ".git",
    ".github",
    ".local",
    ".upstream",
    "autom4te.cache",
    "build",
    "build_msvc",
    "dist",
    "tmp",
    "obj",
    ".deps",
    ".libs",
}

IGNORED_FILE_NAMES = {
    "config.log",
    "config.status",
    "libtool",
}

TEXT_LIKE_EXTENSIONS = {
    ".ac",
    ".am",
    ".bash",
    ".bat",
    ".bzl",
    ".c",
    ".cc",
    ".cfg",
    ".cmake",
    ".conf",
    ".cpp",
    ".css",
    ".csv",
    ".h",
    ".hpp",
    ".in",
    ".ini",
    ".json",
    ".m4",
    ".md",
    ".mm",
    ".nsi",
    ".pc",
    ".pl",
    ".policy",
    ".pro",
    ".ps1",
    ".py",
    ".qrc",
    ".sh",
    ".svg",
    ".tcl",
    ".test",
    ".toml",
    ".ts",
    ".txt",
    ".ui",
    ".xml",
    ".yml",
    ".yaml",
}

ALWAYS_INCLUDE_FILE_NAMES = {
    "Makefile",
    "CMakeLists.txt",
    "configure.ac",
    "README.md",
    ".gitignore",
    ".gitattributes",
}

MAX_FILE_SIZE_BYTES = 8 * 1024 * 1024

FEATURE_GROUPS = {
    "yespower_pow": ("yespower", "hash_x11", "x11"),
    "masternodes_llmq": (
        "masternode",
        "llmq",
        "quorum",
        "chainlock",
        "instantsend",
        "spork",
        "governance",
        "dashbls",
        "evo",
        "protx",
    ),
    "privacy_coinjoin": ("coinjoin", "privatesend", "mixing"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a Smartiecoin vs Bitcoin Core gap report.",
    )
    parser.add_argument(
        "--smartie-root",
        default=str(Path(__file__).resolve().parents[3]),
        help="Path to Smartiecoin root (default: repository root).",
    )
    parser.add_argument(
        "--bitcoin-root",
        required=True,
        help="Path to upstream Bitcoin Core checkout (for example .upstream/bitcoin-30.2).",
    )
    parser.add_argument(
        "--output-md",
        required=True,
        help="Markdown output path.",
    )
    parser.add_argument(
        "--output-json",
        required=True,
        help="JSON output path.",
    )
    parser.add_argument(
        "--max-list",
        type=int,
        default=80,
        help="Maximum entries shown per list in markdown (default: 80).",
    )
    return parser.parse_args()


def relpath(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def include_relpath(rel: str) -> bool:
    parts = rel.split("/")
    if not parts:
        return False
    if parts[0] not in RELEVANT_TOP_LEVEL_DIRS:
        return False
    if any(part in IGNORED_DIR_NAMES for part in parts[:-1]):
        return False
    filename = parts[-1]
    if filename in IGNORED_FILE_NAMES:
        return False
    if filename in ALWAYS_INCLUDE_FILE_NAMES:
        return True
    suffix = Path(filename).suffix.lower()
    return suffix in TEXT_LIKE_EXTENSIONS


def hash_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def scan_repo(root: Path) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for current, dirnames, filenames in os.walk(root):
        current_path = Path(current)
        rel_current = relpath(current_path, root) if current_path != root else ""
        rel_parts = rel_current.split("/") if rel_current else []
        dirnames[:] = [
            d for d in dirnames
            if d not in IGNORED_DIR_NAMES and d != ".git"
        ]
        if rel_parts and rel_parts[0] not in RELEVANT_TOP_LEVEL_DIRS:
            dirnames[:] = []
            continue

        for filename in filenames:
            file_path = current_path / filename
            rel = relpath(file_path, root)
            if not include_relpath(rel):
                continue
            try:
                if file_path.stat().st_size > MAX_FILE_SIZE_BYTES:
                    continue
            except OSError:
                continue
            try:
                result[rel] = hash_file(file_path)
            except OSError:
                continue
    return result


def top_level_counter(paths: Iterable[str]) -> Dict[str, int]:
    counter: Dict[str, int] = collections.Counter()
    for p in paths:
        counter[p.split("/", 1)[0]] += 1
    return dict(sorted(counter.items(), key=lambda x: (-x[1], x[0])))


def subdir_counter(paths: Iterable[str]) -> Dict[str, int]:
    counter: Dict[str, int] = collections.Counter()
    for p in paths:
        parts = p.split("/")
        if len(parts) >= 3:
            key = f"{parts[0]}/{parts[1]}"
        elif len(parts) >= 1:
            key = f"{parts[0]}/(root)"
        else:
            continue
        counter[key] += 1
    return dict(sorted(counter.items(), key=lambda x: (-x[1], x[0])))


def find_feature_paths(paths: Sequence[str], keywords: Sequence[str]) -> List[str]:
    lower_keywords = tuple(k.lower() for k in keywords)
    matches: List[str] = []
    for p in paths:
        p_lower = p.lower()
        if any(k in p_lower for k in lower_keywords):
            matches.append(p)
    return sorted(matches)


def render_counter_table(counter: Dict[str, int], title_a: str, title_b: str) -> List[str]:
    lines = [f"| {title_a} | {title_b} |", "| --- | ---: |"]
    for key, value in counter.items():
        lines.append(f"| `{key}` | {value} |")
    return lines


def render_path_list(paths: Sequence[str], max_items: int) -> List[str]:
    lines: List[str] = []
    shown = list(paths[:max_items])
    for p in shown:
        lines.append(f"- `{p}`")
    if len(paths) > max_items:
        lines.append(f"- ... ({len(paths) - max_items} more)")
    return lines


def make_markdown(
    *,
    smartie_root: Path,
    bitcoin_root: Path,
    only_smartie: List[str],
    only_bitcoin: List[str],
    common_same: List[str],
    common_diff: List[str],
    feature_hits: Dict[str, Dict[str, List[str]]],
    max_list: int,
) -> str:
    now = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
    summary = {
        "smartie_only": len(only_smartie),
        "bitcoin_only": len(only_bitcoin),
        "common_same": len(common_same),
        "common_diff": len(common_diff),
        "total_smartie_indexed": len(only_smartie) + len(common_same) + len(common_diff),
        "total_bitcoin_indexed": len(only_bitcoin) + len(common_same) + len(common_diff),
    }

    lines: List[str] = []
    lines.append("# Smartiecoin vs Bitcoin v30.2 Gap Report")
    lines.append("")
    lines.append(f"- Generated: `{now}`")
    lines.append(f"- Smartie root: `{smartie_root}`")
    lines.append(f"- Bitcoin root: `{bitcoin_root}`")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.extend(
        [
            "| Metric | Count |",
            "| --- | ---: |",
            f"| Indexed files in Smartie | {summary['total_smartie_indexed']} |",
            f"| Indexed files in Bitcoin v30.2 | {summary['total_bitcoin_indexed']} |",
            f"| Smartie-only files | {summary['smartie_only']} |",
            f"| Bitcoin-only files | {summary['bitcoin_only']} |",
            f"| Common files (same content) | {summary['common_same']} |",
            f"| Common files (different content) | {summary['common_diff']} |",
        ]
    )
    lines.append("")
    lines.append("## High-Impact Areas")
    lines.append("")
    lines.append("### Common files with content drift by subdirectory")
    lines.append("")
    lines.extend(render_counter_table(subdir_counter(common_diff), "Subdirectory", "Diff files"))
    lines.append("")
    lines.append("### Smartie-only files by top-level area")
    lines.append("")
    lines.extend(render_counter_table(top_level_counter(only_smartie), "Area", "Files"))
    lines.append("")
    lines.append("### Bitcoin-only files by top-level area")
    lines.append("")
    lines.extend(render_counter_table(top_level_counter(only_bitcoin), "Area", "Files"))
    lines.append("")
    lines.append("## Feature Preservation Surfaces")
    lines.append("")
    for group_name, bucket in feature_hits.items():
        lines.append(f"### `{group_name}`")
        lines.append("")
        lines.append(f"- Smartie-only matches: `{len(bucket['only_smartie'])}`")
        lines.append(f"- Diverged common matches: `{len(bucket['common_diff'])}`")
        lines.append("")
        if bucket["only_smartie"]:
            lines.append("Smartie-only sample:")
            lines.extend(render_path_list(bucket["only_smartie"], max_list))
            lines.append("")
        if bucket["common_diff"]:
            lines.append("Diverged sample:")
            lines.extend(render_path_list(bucket["common_diff"], max_list))
            lines.append("")

    lines.append("## Suggested Next Port Steps")
    lines.append("")
    lines.append("1. Port shared utility/runtime drift first (`src/common`, `src/util`, `src/kernel`, RPC helpers) before consensus-heavy modules.")
    lines.append("2. Keep Smartie feature islands isolated: YesPower PoW, Masternodes/LLMQ, CoinJoin.")
    lines.append("3. Re-run this report after each batch and reduce `common_diff` in core directories while preserving feature islands.")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    smartie_root = Path(args.smartie_root).resolve()
    bitcoin_root = Path(args.bitcoin_root).resolve()
    output_md = Path(args.output_md).resolve()
    output_json = Path(args.output_json).resolve()

    if not smartie_root.is_dir():
        raise SystemExit(f"smartie root does not exist: {smartie_root}")
    if not bitcoin_root.is_dir():
        raise SystemExit(f"bitcoin root does not exist: {bitcoin_root}")

    smartie = scan_repo(smartie_root)
    bitcoin = scan_repo(bitcoin_root)

    smartie_paths = set(smartie.keys())
    bitcoin_paths = set(bitcoin.keys())

    only_smartie = sorted(smartie_paths - bitcoin_paths)
    only_bitcoin = sorted(bitcoin_paths - smartie_paths)
    common = sorted(smartie_paths & bitcoin_paths)
    common_same = [p for p in common if smartie[p] == bitcoin[p]]
    common_diff = [p for p in common if smartie[p] != bitcoin[p]]

    feature_hits: Dict[str, Dict[str, List[str]]] = {}
    for group_name, keywords in FEATURE_GROUPS.items():
        feature_hits[group_name] = {
            "only_smartie": find_feature_paths(only_smartie, keywords),
            "common_diff": find_feature_paths(common_diff, keywords),
        }

    payload = {
        "generated_at_utc": datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z",
        "smartie_root": str(smartie_root),
        "bitcoin_root": str(bitcoin_root),
        "counts": {
            "indexed_smartie": len(smartie_paths),
            "indexed_bitcoin": len(bitcoin_paths),
            "only_smartie": len(only_smartie),
            "only_bitcoin": len(only_bitcoin),
            "common_same": len(common_same),
            "common_diff": len(common_diff),
        },
        "top_level": {
            "only_smartie": top_level_counter(only_smartie),
            "only_bitcoin": top_level_counter(only_bitcoin),
            "common_diff": top_level_counter(common_diff),
        },
        "subdirectory_common_diff": subdir_counter(common_diff),
        "feature_hits": feature_hits,
        "lists": {
            "only_smartie": only_smartie,
            "only_bitcoin": only_bitcoin,
            "common_diff": common_diff,
        },
        "samples": {
            "only_smartie": only_smartie[:300],
            "only_bitcoin": only_bitcoin[:300],
            "common_diff": common_diff[:500],
        },
    }

    output_md.parent.mkdir(parents=True, exist_ok=True)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_md.write_text(
        make_markdown(
            smartie_root=smartie_root,
            bitcoin_root=bitcoin_root,
            only_smartie=only_smartie,
            only_bitcoin=only_bitcoin,
            common_same=common_same,
            common_diff=common_diff,
            feature_hits=feature_hits,
            max_list=args.max_list,
        ),
        encoding="utf-8",
    )
    output_json.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")

    print(f"Wrote markdown report: {output_md}")
    print(f"Wrote JSON report: {output_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
