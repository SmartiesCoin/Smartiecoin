#!/usr/bin/env python3
"""
Generate a phase-2 runtime worklist from a gap-report JSON file.
"""

from __future__ import annotations

import argparse
import collections
import datetime
import json
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


TARGET_PREFIXES = (
    "src/common/",
    "src/util/",
    "src/kernel/",
    "src/node/",
    "src/interfaces/",
    "src/ipc/",
    "src/policy/",
    "src/rpc/",
    "src/script/",
    "src/consensus/",
)

FEATURE_KEYWORDS = (
    "yespower",
    "x11",
    "hash_x11",
    "masternode",
    "llmq",
    "quorum",
    "chainlock",
    "instantsend",
    "spork",
    "governance",
    "coinjoin",
    "privatesend",
    "dashbls",
    "evo",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate phase-2 port worklist from gap report JSON.")
    parser.add_argument("--input-json", required=True, help="Gap report JSON path.")
    parser.add_argument("--output-md", required=True, help="Output markdown path.")
    parser.add_argument("--max-list", type=int, default=200, help="Maximum list entries per section.")
    return parser.parse_args()


def is_target_path(path: str) -> bool:
    return any(path.startswith(prefix) for prefix in TARGET_PREFIXES)


def is_feature_path(path: str) -> bool:
    p = path.lower()
    return any(keyword in p for keyword in FEATURE_KEYWORDS)


def by_subdir(paths: Iterable[str]) -> Dict[str, int]:
    counter: Dict[str, int] = collections.Counter()
    for p in paths:
        parts = p.split("/")
        key = "/".join(parts[:2]) if len(parts) >= 2 else p
        counter[key] += 1
    return dict(sorted(counter.items(), key=lambda x: (-x[1], x[0])))


def render_counter_table(counter: Dict[str, int]) -> List[str]:
    lines = ["| Subdirectory | Files |", "| --- | ---: |"]
    for key, value in counter.items():
        lines.append(f"| `{key}` | {value} |")
    return lines


def render_list(paths: Sequence[str], limit: int) -> List[str]:
    lines: List[str] = []
    for p in paths[:limit]:
        lines.append(f"- `{p}`")
    if len(paths) > limit:
        lines.append(f"- ... ({len(paths) - limit} more)")
    return lines


def main() -> int:
    args = parse_args()
    input_json = Path(args.input_json).resolve()
    output_md = Path(args.output_md).resolve()

    data = json.loads(input_json.read_text(encoding="utf-8"))
    lists = data["lists"]

    common_diff = lists["common_diff"]
    only_bitcoin = lists["only_bitcoin"]

    common_runtime = sorted([p for p in common_diff if is_target_path(p) and not is_feature_path(p)])
    bitcoin_only_runtime = sorted([p for p in only_bitcoin if is_target_path(p) and not is_feature_path(p)])

    now = datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z"
    lines: List[str] = []
    lines.append("# Bitcoin v30.2 Phase-2 Runtime Worklist")
    lines.append("")
    lines.append(f"- Generated: `{now}`")
    lines.append(f"- Source report: `{input_json}`")
    lines.append("")
    lines.append("## Scope")
    lines.append("")
    lines.append("This worklist focuses on runtime/core alignment and excludes Smartie feature islands (YesPower, Masternodes/LLMQ, CoinJoin).")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append("| Metric | Count |")
    lines.append("| --- | ---: |")
    lines.append(f"| Runtime common-diff candidates | {len(common_runtime)} |")
    lines.append(f"| Runtime bitcoin-only candidates | {len(bitcoin_only_runtime)} |")
    lines.append("")
    lines.append("## Runtime Common-Diff by Subdirectory")
    lines.append("")
    lines.extend(render_counter_table(by_subdir(common_runtime)))
    lines.append("")
    lines.append("## Runtime Bitcoin-Only by Subdirectory")
    lines.append("")
    lines.extend(render_counter_table(by_subdir(bitcoin_only_runtime)))
    lines.append("")
    lines.append("## Candidate Files: Common-Diff")
    lines.append("")
    lines.extend(render_list(common_runtime, args.max_list))
    lines.append("")
    lines.append("## Candidate Files: Bitcoin-Only")
    lines.append("")
    lines.extend(render_list(bitcoin_only_runtime, args.max_list))
    lines.append("")

    output_md.parent.mkdir(parents=True, exist_ok=True)
    output_md.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote phase-2 worklist: {output_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

