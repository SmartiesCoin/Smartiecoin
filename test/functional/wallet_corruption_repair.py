#!/usr/bin/env python3
# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Preserve recovered keys and unmatched raw transaction data after a rescan."""

import hashlib
from pathlib import Path
import subprocess

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet


class WalletCorruptionRepairTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument('--database', choices=['sqlite', 'bdb'], default='sqlite')
        parser.add_argument('--scenario', choices=['all', 'unmatched', 'recovered'], default='all')

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.bind_to_localhost_only = False  # No P2P listener at all.
        self.extra_args = [['-listen=0', '-connect=0', '-dnsseed=0', '-discover=0', '-keypool=2']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_wallet_tool()
        if self.options.database == 'bdb':
            self.skip_if_no_bdb()
        else:
            self.skip_if_no_sqlite()

    def setup_nodes(self):
        self.add_nodes(self.num_nodes, extra_args=self.extra_args)
        self.start_nodes()

    def wallet_tool(self, *args):
        subprocess.run([self.options.bitcoinwallet, f'-datadir={self.nodes[0].datadir}',
                        '-regtest', *args], check=True, capture_output=True, text=True)

    def run_test(self):
        if self.options.scenario in ('all', 'unmatched'):
            self.test_unmatched_records()
        if self.options.scenario in ('all', 'recovered'):
            self.test_recovered_keys()

    def test_unmatched_records(self):
        node = self.nodes[0]
        node.createwallet('source', descriptors=False, load_on_startup=False)
        wallet = node.get_wallet_rpc('source')
        miner = MiniWallet(node)
        self.generatetoaddress(node, 101, miner.get_address())
        miner.rescan_utxos()
        address = wallet.getnewaddress()
        script = bytes.fromhex(node.validateaddress(address)['scriptPubKey'])
        confirmed = miner.send_to(from_node=node, scriptPubKey=script, amount=1000000)['txid']
        self.generatetoaddress(node, 1, miner.get_address())
        unconfirmed = miner.send_to(from_node=node, scriptPubKey=script, amount=2000000)['txid']
        assert_equal(wallet.gettransaction(unconfirmed)['confirmations'], 0)
        assert_equal(wallet.gettransaction(confirmed)['confirmations'], 1)
        # Move the wallet birthday past the confirmed transaction. The startup
        # scan must succeed but cannot visit its block, even with -rescan=1.
        self.bump_mocktime(3 * 60 * 60, update_schedulers=False)
        birthday = self.mocktime
        node.unloadwallet('source')
        # Do not mine the unconfirmed transaction: create later empty blocks
        # after restarting without a persisted mempool or a loaded source wallet.
        self.stop_node(0)
        self.start_node(0, extra_args=self.extra_args[0] + ['-persistmempool=0'])
        assert_equal(node.getrawmempool(), [])
        later_blocks = self.generatetoaddress(node, 2, miner.get_address())
        self.stop_node(0)

        dump = Path(node.datadir) / 'source.dump'
        self.wallet_tool('-wallet=source', f'-dumpfile={dump}', 'dump')
        records = dict(line.split(',', 1) for line in dump.read_text().splitlines())
        records['format'] = self.options.database
        preserved = {}
        for txid, bad_byte in ((unconfirmed, '11'), (confirmed, '22')):
            real_key = '027478' + bytes.fromhex(txid)[::-1].hex()
            bad_key = '027478' + bad_byte * 32
            assert bad_key not in records
            preserved[bad_key] = records.pop(real_key)
            records[bad_key] = preserved[bad_key]
            # The mismatched record is the only copy of this transaction in
            # this fixture; neither its true key nor any duplicate remains.
            assert_equal(list(records.values()).count(preserved[bad_key]), 1)
        metadata_keys = [key for key in records if key.startswith('076b65796d657461')]
        assert metadata_keys
        for key in metadata_keys:
            value = bytes.fromhex(records[key])
            records[key] = (value[:4] + birthday.to_bytes(8, 'little', signed=True) + value[12:]).hex()
        body = ''.join(f'{key},{value}\n' for key, value in records.items() if key != 'checksum')
        checksum = hashlib.sha256(hashlib.sha256(body.encode()).digest()).hexdigest()
        fixture_dump = Path(node.datadir) / 'unmatched.dump'
        fixture_dump.write_bytes((body + f'checksum,{checksum}\n').encode())
        self.wallet_tool('-wallet=unmatched', f'-dumpfile={fixture_dump}',
                         f'-format={self.options.database}', 'createfromdump')
        wallet_path = Path(node.datadir) / 'regtest' / 'wallets' / 'unmatched' / 'wallet.dat'
        warning = (f'Warning: Error reading {wallet_path}! '
                   'All keys read correctly, but transaction data or address book entries might be missing or incorrect.')
        for attempt in range(2):
            self.start_node(0, extra_args=self.extra_args[0] +
                            ['-wallet=unmatched', '-persistmempool=0', '-rescan=1'])
            wallet = node.get_wallet_rpc('unmatched')
            assert_equal(wallet.getwalletinfo()['format'], self.options.database)
            assert_equal(wallet.getwalletinfo()['txcount'], 0)
            assert_equal(node.getrawmempool(), [])
            for txid in (unconfirmed, confirmed):
                assert_raises_rpc_error(-5, 'Invalid or non-wallet transaction id', wallet.gettransaction, txid)
            # Startup completed, and the two later blocks actually were scanned.
            debug_log = (Path(node.datadir) / 'regtest' / 'debug.log').read_text()
            assert_equal(debug_log.count(f'[unmatched] Rescan started from block {later_blocks[0]}'), attempt + 1)
            assert_equal(debug_log.count('[unmatched] Rescan completed'), attempt + 1)
            assert_equal(node.getbestblockhash(), later_blocks[-1])
            self.stop_node(0, expected_stderr=warning)
            after_dump = Path(node.datadir) / f'unmatched-after-{attempt}.dump'
            self.wallet_tool('-wallet=unmatched', f'-dumpfile={after_dump}', 'dump')
            after = dict(line.split(',', 1) for line in after_dump.read_text().splitlines())
            missing = [key for key, value in preserved.items() if after.get(key) != value]
            assert not missing, f'Lost sole raw transaction copies (unconfirmed / outside birthday scan): {missing}'
        self.start_node(0, extra_args=self.extra_args[0])

    def test_recovered_keys(self):
        node = self.nodes[0]
        node.createwallet('original', descriptors=False, load_on_startup=False)
        wallet = node.get_wallet_rpc('original')
        blocks = self.generatetoaddress(node, 2, wallet.getnewaddress())
        txids = [node.getblock(block)['tx'][0] for block in blocks]
        expected = {txid: wallet.gettransaction(txid)['hex'] for txid in txids}
        self.stop_node(0)

        # Work only on disposable dump-derived fixtures, preserving every key and
        # the original wallet. Each fixture is created in the requested backend;
        # no existing database is converted or edited while the node is running.
        dump = Path(node.datadir) / 'original.dump'
        self.wallet_tool('-wallet=original', f'-dumpfile={dump}', 'dump')
        records = dict(line.split(',', 1) for line in dump.read_text().splitlines())
        tx_keys = ['027478' + bytes.fromhex(txid)[::-1].hex() for txid in txids]
        assert all(key in records for key in tx_keys)
        records['format'] = self.options.database
        records[tx_keys[0]] = records[tx_keys[1]]
        # Unmatched raw records must survive, even when another copy exists:
        # a successful scan is not proof that all transaction data was recovered.
        stale_key = '027478' + '00' * 32
        assert stale_key not in records
        records[stale_key] = records[tx_keys[1]]
        # The key is still transaction A, but the serialized value is transaction B.
        body = ''.join(f'{key},{value}\n' for key, value in records.items() if key != 'checksum')
        checksum = hashlib.sha256(hashlib.sha256(body.encode()).digest()).hexdigest()
        corrupt_dump = Path(node.datadir) / 'corrupt.dump'
        # Use LF bytes on every platform: the dump checksum covers exact bytes.
        corrupt_dump.write_bytes((body + f'checksum,{checksum}\n').encode())
        self.wallet_tool('-wallet=repaired', f'-dumpfile={corrupt_dump}',
                         f'-format={self.options.database}', 'createfromdump')

        self.start_node(0, extra_args=self.extra_args[0] + ['-wallet=repaired'])
        wallet = node.get_wallet_rpc('repaired')
        assert_equal(wallet.getwalletinfo()['format'], self.options.database)
        assert_equal(wallet.getwalletinfo()['txcount'], 2)
        for txid in txids:
            assert_equal(wallet.gettransaction(txid)['hex'], expected[txid])
        self.log.info('Rescan recovered both transactions; checking persistence across restart')
        wallet_path = Path(node.datadir) / 'regtest' / 'wallets' / 'repaired' / 'wallet.dat'
        warning = (f'Warning: Error reading {wallet_path}! '
                   'All keys read correctly, but transaction data or address book entries might be missing or incorrect.')
        self.stop_node(0, expected_stderr=warning)
        repaired_dump = Path(node.datadir) / 'repaired.dump'
        self.wallet_tool('-wallet=repaired', f'-dumpfile={repaired_dump}', 'dump')
        repaired_records = dict(line.split(',', 1) for line in repaired_dump.read_text().splitlines())
        assert_equal(repaired_records.get(stale_key), records[stale_key])
        assert all(key in repaired_records for key in tx_keys)
        assert_equal(repaired_records['format'], self.options.database)
        self.start_node(0, extra_args=self.extra_args[0] + ['-wallet=repaired'])
        wallet = node.get_wallet_rpc('repaired')
        assert_equal(wallet.getwalletinfo()['txcount'], 2)
        for txid in txids:
            assert_equal(wallet.gettransaction(txid)['hex'], expected[txid])
        self.stop_node(0, expected_stderr=warning)


if __name__ == '__main__':
    WalletCorruptionRepairTest().main()
