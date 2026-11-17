#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regression test for the msghand CPU-exhaustion DoS (Gatevia report, v0.5.3).

CBlockHeader::GetHash() is memory-hard yespower (~3 ms with pre-v0.5.0 params,
which an attacker can force via the nTime field). Before the fix, an inbound
peer could replay already-known valid headers and force ~2 yespower hashes per
header (CheckHeadersAreContinuous + AcceptBlockHeader) with no punishment,
pegging the msghand thread and starving RPC (cs_main).

This test mines a chain, then replays 4 messages of 2000 known continuous
headers from an inbound connection and requires the node to answer a ping
promptly afterwards. On the unfixed binary the pong is queued behind tens of
seconds of yespower work and the test times out; with the header-hash cache it
completes in about a second.
"""

import time
from io import BytesIO

from test_framework.messages import (
    CBlockHeader,
    msg_headers,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class PowHashReplayTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # The debug consistency walk is unrelated to hash cost; keep the test
        # focused on PoW hashing (it is O(chain) per header on regtest).
        self.extra_args = [["-checkblockindex=0"]]

    def run_test(self):
        node = self.nodes[0]
        nblocks = 2000
        self.log.info(f"Mining {nblocks} regtest blocks (known-header corpus)...")
        # Small chunks: regtest PoW here is the expensive 8 MiB yespower variant
        # (fork disabled on regtest), so each block costs milliseconds of CPU.
        for _ in range(nblocks // 50):
            self.generate(node, 50)
        assert_equal(node.getblockcount(), nblocks)

        # Walk back from the tip collecting raw headers (one RPC per block).
        self.log.info("Collecting known headers...")
        raw_headers = []
        h = node.getbestblockhash()
        for _ in range(nblocks):
            raw = node.getblockheader(h, False)
            raw_headers.append(raw)
            # hashPrevBlock is bytes 4..36, little-endian on the wire.
            h = bytes.fromhex(raw[8:72])[::-1].hex()
        raw_headers.reverse()
        headers = []
        for raw in raw_headers:
            bh = CBlockHeader()
            bh.deserialize(BytesIO(bytes.fromhex(raw)))
            # Pre-warm the framework's PoW memo: dashhash() spawns
            # smartiecoin-util per unique header (~8 ms each). Doing it here
            # keeps subprocess cost out of the measured flood window.
            bh.calc_sha256()
            headers.append(bh)

        peer = node.add_p2p_connection(P2PInterface())
        flood = msg_headers(headers=headers)  # 2000 known, continuous headers

        self.log.info("Replaying 4 x 2000 known headers from inbound peer...")
        start = time.time()
        for _ in range(4):
            peer.send_message(flood)
        # The pong is processed on msghand AFTER the queued headers messages,
        # so its latency measures the head-of-line CPU cost directly.
        peer.sync_with_ping(timeout=25)
        elapsed = time.time() - start
        self.log.info(f"Flood + ping round-trip took {elapsed:.2f}s")

        assert elapsed < 20, f"msghand blocked {elapsed:.1f}s on replayed known headers (DoS)"
        assert peer.is_connected, "peer must not be punished for valid known headers"
        assert_equal(node.getblockcount(), nblocks)
        # Node still fully responsive after the flood.
        assert_equal(node.getblockhash(nblocks), node.getbestblockhash())


if __name__ == '__main__':
    PowHashReplayTest().main()
