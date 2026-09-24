#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Unit regressions for masternode test setup (no daemon required)."""

from types import SimpleNamespace
import unittest
from unittest.mock import Mock

from test_framework.test_framework import DashTestFramework


class TestMasternodeHelpers(unittest.TestCase):
    def test_waits_for_every_participating_masternode(self):
        def masternode(connected):
            status = {
                'session': [{'llmqType': 'llmq_test', 'status': {'quorumHash': 'quorum'}}],
                'quorumConnections': [{
                    'llmqType': 'llmq_test', 'quorumHash': 'quorum',
                    'quorumConnections': [{'connected': connected}, {'connected': connected}],
                }],
            }
            return SimpleNamespace(get_node=lambda framework: SimpleNamespace(quorum=lambda command: status))

        observed = []
        framework = SimpleNamespace(wait_until=lambda predicate, **kwargs: observed.append(predicate()))
        # One ready member must not hide the unready second member.
        DashTestFramework.wait_for_quorum_connections(framework, 'quorum', 2,
                                                     [masternode(True), masternode(False)])
        self.assertEqual(observed, [False])
        observed.clear()
        DashTestFramework.wait_for_quorum_connections(framework, 'quorum', 2,
                                                     [masternode(True), masternode(True)])
        self.assertEqual(observed, [True])

    def test_large_block_moves_keep_rpc_batches_bounded(self):
        nodes = [object()]
        mined = []
        sync = Mock()
        def generate(node, count, sync_fun):
            mined.append(count)
            sync_fun()
        framework = SimpleNamespace(nodes=nodes, bump_mocktime=Mock(), generate=generate,
                                    sync_blocks=sync, no_op=lambda: None)
        DashTestFramework.move_blocks(framework, nodes, 77)
        self.assertEqual(sum(mined), 77)
        self.assertLessEqual(max(mined), 10)
        framework.bump_mocktime.assert_called_once_with(1, nodes=nodes)
        sync.assert_called_once_with(nodes)


if __name__ == '__main__':
    unittest.main()
