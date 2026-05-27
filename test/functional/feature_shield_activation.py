#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test SMT v0.3.5 shielded transaction activation guards."""

import os
import struct
from decimal import Decimal

from test_framework.messages import OUTPUT_DESCRIPTION_SIZE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def v4_tx_hex(value_balance=0, shielded_outputs=0, transparent=True):
    """Build a minimal v4 tx with Sapling fields."""
    script_pubkey = bytes.fromhex("76a914" + "11" * 20 + "88ac")
    tx = bytearray()
    tx += struct.pack("<I", 4)
    if transparent:
        tx += b"\x01"                         # vin count
        tx += bytes.fromhex("01" * 32)          # dummy non-null txid
        tx += struct.pack("<I", 0)              # output index
        tx += b"\x00"                         # empty scriptSig
        tx += struct.pack("<I", 0xffffffff)     # sequence
        tx += b"\x01"                         # vout count
        tx += struct.pack("<q", 1000)           # value
        tx += bytes([len(script_pubkey)])
        tx += script_pubkey
    else:
        tx += b"\x00"                         # vin count
        tx += b"\x00"                         # vout count
    tx += struct.pack("<I", 0)              # locktime
    tx += struct.pack("<q", value_balance)  # Sapling valueBalance
    tx += b"\x00"                         # shielded spends
    tx += bytes([shielded_outputs])         # shielded outputs
    tx += bytes(OUTPUT_DESCRIPTION_SIZE * shielded_outputs)
    tx += bytes(64)                         # bindingSig
    return tx.hex()


class ShieldActivationTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.params_args = [f"-paramsdir={os.path.join(self.config['environment']['SRCDIR'], 'params')}"]
        self.extra_args = [self.params_args]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_reject_reason(self, rawtx, reason):
        result = self.nodes[0].testmempoolaccept([rawtx])[0]
        assert_equal(result["allowed"], False)
        assert_equal(result["reject-reason"], reason)

    def assert_amount(self, actual, expected):
        assert_equal(Decimal(str(actual)), Decimal(expected))

    def test_wallet_shielded_flow(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="shield_smoke", descriptors=False)
        wallet = node.get_wallet_rpc("shield_smoke")

        transparent_source = wallet.getnewaddress()
        self.generatetoaddress(node, 110, transparent_source)

        shielded_address = wallet.getnewshieldaddress()
        sapling_key = wallet.exportsaplingkey(shielded_address)
        viewing_key = wallet.exportsaplingviewingkey(shielded_address)
        assert shielded_address in wallet.listshieldaddresses()
        assert sapling_key.startswith("rsmtsecret")
        assert viewing_key.startswith("rsmtview")

        self.log.info("Exercise transparent-to-shielded wallet send")
        tx1 = wallet.sendtoshieldaddress(shielded_address, Decimal("1"), 1, Decimal("0.001"), True)["txid"]
        self.generatetoaddress(node, 1, transparent_source)
        self.assert_amount(wallet.getshieldbalance(), "1.00000000")
        notes = wallet.listshieldunspent()
        assert_equal(len(notes), 1)
        self.assert_amount(notes[0]["amount"], "1.00000000")

        self.log.info("Exercise shielded-to-transparent wallet send")
        transparent_recipient = wallet.getnewaddress()
        tx2 = wallet.sendfromshieldaddress("*", transparent_recipient, Decimal("0.25"), 1, Decimal("0.001"), True)["txid"]
        self.generatetoaddress(node, 1, transparent_source)
        self.assert_amount(wallet.getreceivedbyaddress(transparent_recipient, 1), "0.25000000")
        self.assert_amount(wallet.getshieldbalance(), "0.74900000")

        self.log.info("Exercise shielded-to-shielded wallet send")
        shielded_recipient = wallet.getnewshieldaddress()
        tx3 = wallet.sendfromshieldaddress("*", shielded_recipient, Decimal("0.10"), 1, Decimal("0.001"), True)["txid"]
        self.generatetoaddress(node, 1, transparent_source)
        self.assert_amount(wallet.getshieldbalance(), "0.74800000")
        notes = wallet.listshieldunspent()
        assert any(
            note["address"] == shielded_recipient and Decimal(str(note["amount"])) == Decimal("0.10000000")
            for note in notes
        )

        for txid in (tx1, tx2, tx3):
            assert_equal(node.getrawtransaction(txid, True)["version"], 4)

    def run_test(self):
        raw_empty_v4 = v4_tx_hex()
        raw_payload_v4 = v4_tx_hex(value_balance=-100, shielded_outputs=1, transparent=False)

        self.log.info("Reject v4 transactions before shield activation")
        assert_equal(self.nodes[0].getblockchaininfo()["softforks"]["shield"], {
            "type": "buried",
            "active": False,
            "height": 999999999,
        })
        self.assert_reject_reason(raw_empty_v4, "bad-txns-shield-not-active")

        self.log.info("Allow v4 relay policy after shield activation guard passes")
        self.restart_node(0, extra_args=self.params_args + ["-testactivationheight=shield@0"])
        assert_equal(self.nodes[0].getblockchaininfo()["softforks"]["shield"], {
            "type": "buried",
            "active": True,
            "height": 0,
        })
        self.assert_reject_reason(raw_empty_v4, "missing-inputs")

        self.log.info("Reject malformed shielded payloads through Sapling proof validation")
        self.assert_reject_reason(raw_payload_v4, "bad-txns-sapling-output-description-invalid")

        self.test_wallet_shielded_flow()


if __name__ == "__main__":
    ShieldActivationTest().main()
