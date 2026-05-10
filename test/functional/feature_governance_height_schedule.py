#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test height-based governance proposal payouts survive wall-clock expiration."""

from test_framework.governance import have_trigger_for_height, prepare_object
from test_framework.messages import uint256_to_string
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, satoshi_round


class GovernanceHeightScheduleTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(2, 1, [["-budgetparams=10:10:10"]] * 2)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_9_SUPERBLOCKS_ENABLED", 0)
        self.wait_for_sporks_same()

        info = self.nodes[0].getgovernanceinfo()
        cycle = info["superblockcycle"]
        maturity_window = info["superblockmaturitywindow"]
        current_height = self.nodes[0].getblockcount()
        target_height = ((current_height // cycle) + 2) * cycle

        proposal_time = self.mocktime
        payout_address = self.nodes[0].getnewaddress()
        payout_amount = satoshi_round("1.1")
        prepared = prepare_object(
            self.nodes[0], 1, uint256_to_string(0), proposal_time, 1, "HeightPay",
            payout_amount, payout_address, end_epoch=proposal_time + 20 * 60,
            payment_height=target_height, payment_count=1)

        self.bump_mocktime(10 * 60 + 1)
        self.generate(self.nodes[0], 6, sync_fun=self.sync_blocks())

        proposal_hash = self.nodes[0].gobject(
            "submit", "0", 1, proposal_time, prepared["hex"], prepared["collateralHash"])
        self.nodes[0].gobject("vote-many", proposal_hash, "funding", "yes")
        assert_equal(self.nodes[0].gobject("get", proposal_hash)["FundingResult"]["YesCount"], self.mn_count)

        self.log.info("Expire the proposal wall-clock window before trigger creation")
        for delta in (60 * 60, 60 * 60, 10):
            self.bump_mocktime(delta)

        trigger_start = target_height - maturity_window + 1
        blocks_to_trigger = trigger_start - self.nodes[0].getblockcount()
        assert blocks_to_trigger > 0
        self.generate(self.nodes[0], blocks_to_trigger, sync_fun=self.sync_blocks())
        self.wait_until(lambda: have_trigger_for_height(self.nodes, target_height), timeout=10)

        self.log.info("Mine the scheduled superblock and verify the proposal payout")
        blocks_to_superblock = target_height - self.nodes[0].getblockcount()
        assert blocks_to_superblock > 0
        self.generate(self.nodes[0], blocks_to_superblock, sync_fun=self.sync_blocks())
        assert_equal(self.nodes[0].getblockcount(), target_height)

        coinbase_outputs = self.nodes[0].getblock(self.nodes[0].getbestblockhash(), 2)["tx"][0]["vout"]
        assert any(
            txout["value"] == payout_amount and txout["scriptPubKey"]["address"] == payout_address
            for txout in coinbase_outputs
        )


if __name__ == "__main__":
    GovernanceHeightScheduleTest().main()
