#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_is_retroactive.py

Tests retroactive signing

We have 5 nodes where node 0 is the control node, nodes 1-4 are masternodes.
Mempool inconsistencies are simulated via disconnecting/reconnecting node 3
and by having a higher relay fee on node 4.
'''

import time

from test_framework.test_framework import DashTestFramework
from test_framework.util import set_node_times


class LLMQ_IS_RetroactiveSigning(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # -whitelist is needed to avoid the trickling logic on node0
        self.set_dash_test_params(5, 4, [["-whitelist=127.0.0.1"], [], [], [], ["-minrelaytxfee=0.001"]])
        # SMT: v20 a little past the framework's warm-up. The warm-up mines until the controller
        # holds 60,100 SMT (4 x 15,000 collateral + fees). With the stock v20 test height (100)
        # the regtest subsidy is the fixed 5 SMT base, which decays x(13/14) every ~150 blocks
        # and tops out around 56,600 -> the warm-up can never finish. With v20 at 300 the
        # pre-v20 regtest subsidy (~500/block, halved by the v0.1.4 split) reaches the target in
        # ~250 blocks; v20 then activates mid-way through the first mine_cycle_quorum, exactly
        # like upstream dash (v20@100 activates during its own first cycle), and every
        # IS-retroactive assertion below still runs in the post-v20 regime (= mainnet).
        self.delay_v20_and_mn_rr(height=300)
        # SMT: mine regtest blocks with the v0.5.0 cache-tuned PoW (r=8) from just after the
        # test genesis (nTime 1590000000) so warm-up mining is ~10x faster (no functional test
        # asserts the legacy r=32 PoW; unit vectors + the miner e2e cover the fork itself). The
        # fork time MUST stay > the genesis nTime: any value <= it re-hashes the genesis with
        # the new parameters and breaks startup.
        for args in self.extra_args:
            args.append("-testactivationheight=smt050pow@1590000001")

    def assert_no_instantlock(self, txid, node):
        self.log.info(f"Expecting no InstantLock for {txid}")
        assert not node.getrawtransaction(txid, True)["instantlock"]

    def sleep_and_assert_no_instantlock(self, txid, node, sleep=5):
        time.sleep(sleep)
        self.assert_no_instantlock(txid, node)

    # random delay before tx is actually send by network could take up to 30 seconds
    def wait_for_tx(self, txid, node, expected=True, timeout=60):
        def check_tx():
            try:
                return node.getrawtransaction(txid)
            except Exception:
                return False
        if self.wait_until(check_tx, timeout=timeout, do_assert=expected) and not expected:
            raise AssertionError("waiting unexpectedly succeeded")

    def create_fund_sign_tx(self):
        rawtx = self.nodes[0].createrawtransaction([], {self.nodes[0].getnewaddress(): 1})
        rawtx = self.nodes[0].fundrawtransaction(rawtx)['hex']
        rawtx = self.nodes[0].signrawtransactionwithwallet(rawtx)['hex']
        return rawtx

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        # Turn mempool IS signing off
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 1)
        self.wait_for_sporks_same()

        self.mine_cycle_quorum()

        # Make sure that all nodes are chainlocked at the same height before starting actual tests
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash(), timeout=30)

        self.log.info("trying normal IS lock w/ signing spork off. Shouldn't be islocked before block is created.")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        # 3 nodes should be enough to create an IS lock even if nodes 4 and 5 (which have no tx itself)
        # are the only "neighbours" in intra-quorum connections for one of them.
        self.bump_mocktime(30)
        self.sleep_and_assert_no_instantlock(txid, self.nodes[0])
        # Have to disable ChainLocks to avoid signing a block with a "safe" tx too early
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 4000000000)
        self.wait_for_sporks_same()
        # We have to wait in order to include tx in block
        self.bump_mocktime(10 * 60 + 1)
        block = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        self.wait_for_instantlock(txid, self.nodes[0])
        self.nodes[0].sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 0)
        self.wait_for_sporks_same()
        self.wait_for_chainlocked_block_all_nodes(block)

        self.log.info("Enable mempool IS signing")
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 0)
        self.wait_for_sporks_same()

        self.log.info("trying normal IS lock")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        # 3 nodes should be enough to create an IS lock even if nodes 4 and 5 (which have no tx itself)
        # are the only "neighbours" in intra-quorum connections for one of them.
        self.bump_mocktime(30)
        self.wait_for_instantlock(txid, self.nodes[0])
        block = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        self.wait_for_chainlocked_block_all_nodes(block)

        self.log.info("testing normal signing with partially known TX")
        self.isolate_node(3)
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        # Make sure nodes 1 and 2 received the TX before we continue,
        # otherwise it might announce the TX to node 3 when reconnecting
        self.bump_mocktime(30)
        self.wait_for_tx(txid, self.nodes[1])
        self.wait_for_tx(txid, self.nodes[2])
        self.reconnect_isolated_node(3, 0)
        # Make sure nodes actually try re-connecting quorum connections
        self.bump_mocktime(30)
        self.wait_for_mnauth(self.nodes[3], 2)
        # node 3 fully reconnected but the TX wasn't relayed to it, so there should be no IS lock
        self.sleep_and_assert_no_instantlock(txid, self.nodes[0])
        # push the tx directly via rpc
        self.nodes[3].sendrawtransaction(self.nodes[0].getrawtransaction(txid))
        # node 3 should vote on a tx now since it became aware of it via sendrawtransaction
        # and this should be enough to complete an IS lock
        self.wait_for_instantlock(txid, self.nodes[0])

        self.log.info("testing retroactive signing with unknown TX")
        self.isolate_node(3)
        rawtx = self.create_fund_sign_tx()
        txid = self.nodes[3].sendrawtransaction(rawtx)
        # Make node 3 consider the TX as safe
        self.bump_mocktime(10 * 60 + 1)
        block = self.generatetoaddress(self.nodes[3], 1, self.nodes[0].getnewaddress(), sync_fun=self.no_op)[0]
        self.reconnect_isolated_node(3, 0)
        self.wait_for_chainlocked_block_all_nodes(block)
        self.nodes[0].setmocktime(self.mocktime)

        self.log.info("testing retroactive signing with partially known TX")
        self.isolate_node(3)
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        # Make sure nodes 1 and 2 received the TX before we continue,
        # otherwise it might announce the TX to node 3 when reconnecting
        self.bump_mocktime(30)
        self.wait_for_tx(txid, self.nodes[1])
        self.wait_for_tx(txid, self.nodes[2])
        self.reconnect_isolated_node(3, 0)
        # Make sure nodes actually try re-connecting quorum connections
        self.bump_mocktime(30)
        self.wait_for_mnauth(self.nodes[3], 2)
        # node 3 fully reconnected but the TX wasn't relayed to it, so there should be no IS lock
        self.sleep_and_assert_no_instantlock(txid, self.nodes[0])
        # Make node0 consider the TX as safe
        self.bump_mocktime(10 * 60 + 1)
        block = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        assert txid in self.nodes[0].getblock(block, 1)['tx']
        self.wait_for_chainlocked_block_all_nodes(block)

        self.log.info("testing retroactive signing with partially known TX and session timeout")
        self.test_session_timeout(False)
        self.log.info("repeating test, but with cycled LLMQs")
        self.test_session_timeout(True)


    def test_session_timeout(self, do_cycle_llmqs):
        set_node_times(self.nodes, self.mocktime)
        self.isolate_node(3)
        rawtx = self.create_fund_sign_tx()
        txid_all_nodes = self.nodes[0].sendrawtransaction(rawtx)
        txid_all_nodes = self.nodes[3].sendrawtransaction(rawtx)

        rawtx_1 = self.create_fund_sign_tx()
        txid_single_node = self.nodes[3].sendrawtransaction(rawtx_1)

        # Make sure nodes 1 and 2 received the TX before we continue
        self.bump_mocktime(30)
        self.wait_for_tx(txid_all_nodes, self.nodes[1])
        self.wait_for_tx(txid_all_nodes, self.nodes[2])
        # Make sure signing is done on nodes 1 and 2 (it's async)
        time.sleep(5)
        # Make the signing session for the IS lock timeout on nodes 1-3
        self.bump_mocktime(61)
        time.sleep(2) # make sure Cleanup() is called
        self.reconnect_isolated_node(3, 0)
        # Make sure nodes actually try re-connecting quorum connections
        self.bump_mocktime(30)
        self.wait_for_mnauth(self.nodes[3], 2)

        self.nodes[0].sendrawtransaction(rawtx_1)
        # Make sure nodes 1 and 2 received the TX
        self.bump_mocktime(30)
        self.wait_for_tx(txid_single_node, self.nodes[1])
        self.wait_for_tx(txid_single_node, self.nodes[2])
        self.bump_mocktime(30)

        # Make sure signing is done on nodes 1 and 2 (it's async)
        time.sleep(5)

        # node 3 fully reconnected but the signing session is already timed out, so no IS lock
        self.assert_no_instantlock(txid_all_nodes, self.nodes[0])
        self.assert_no_instantlock(txid_single_node, self.nodes[0])
        if do_cycle_llmqs:
            self.mine_cycle_quorum()
            self.mine_cycle_quorum()
            self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash(), timeout=30)

            time.sleep(5)
            self.assert_no_instantlock(txid_all_nodes, self.nodes[0])
            self.assert_no_instantlock(txid_single_node, self.nodes[0])
        # Make node 0 consider the TX as safe
        self.bump_mocktime(10 * 60 + 1)
        block = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        assert txid_all_nodes in self.nodes[0].getblock(block, 1)['tx']
        assert txid_single_node in self.nodes[0].getblock(block, 1)['tx']
        self.wait_for_chainlocked_block_all_nodes(block)

if __name__ == '__main__':
    LLMQ_IS_RetroactiveSigning().main()
