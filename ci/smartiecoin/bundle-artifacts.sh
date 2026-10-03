#!/usr/bin/env bash
# Copyright (c) 2024-2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
export LC_ALL=C
set -euo pipefail

# CI extraction must invoke bundle_archive.py from the trusted base checkout,
# never this PR-controlled wrapper. -I ignores workspace Python imports/env.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -I "${SCRIPT_DIR}/bundle_archive.py" "$@"
