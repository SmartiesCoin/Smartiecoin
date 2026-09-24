#!/usr/bin/env python3
"""Disposable native-miner RPC integration; never connects to an existing node.

Uses only Python stdlib. A transparent loopback RPC recorder forwards real GBT /
submitblock calls; receipts include exact templates, submitted blocks and chain
readback. Bootstrap blocks are RPC-generated, test blocks MUST come from miner.
"""
import argparse
import base64
import ctypes
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

FORK = 1790528400
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def save(path, data):
    Path(path).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def wait_for(fn, timeout=60):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            value = fn()
            if value:
                return value
        except (OSError, RuntimeError) as exc:
            last = exc
        time.sleep(0.05)
    raise AssertionError(f"timeout ({timeout}s): {last}")


class RPC:
    def __init__(self, port, password, wallet=None):
        self.url = f"http://127.0.0.1:{port}/" + (f"wallet/{wallet}" if wallet else "")
        self.auth = base64.b64encode(f"miner-test:{password}".encode()).decode()

    def request(self, req):
        wire = urllib.request.Request(self.url, json.dumps(req).encode(),
                                      {"Content-Type": "application/json", "Authorization": "Basic " + self.auth})
        try:
            response = urllib.request.urlopen(wire, timeout=90)
        except urllib.error.HTTPError as exc:
            response = exc
        return json.loads(response.read())

    def __call__(self, method, *params):
        reply = self.request({"id": 1, "method": method, "params": list(params)})
        if reply.get("error"):
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply["result"]


def read_compact(raw, offset):
    value = raw[offset]
    size = {253: 2, 254: 4, 255: 8}.get(value, 0)
    if size:
        return int.from_bytes(raw[offset + 1:offset + 1 + size], "little"), offset + 1 + size
    return value, offset + 1


def missing_payee_block(raw_hex, tpl):
    """Negative control: move all payees' value to the miner, keep total unchanged."""
    raw = bytes.fromhex(raw_hex)
    _, start = read_compact(raw, 80)
    nvin, pos = read_compact(raw, start + 4)
    assert nvin == 1
    script_len, pos = read_compact(raw, pos + 36)
    pos += script_len + 4
    vout_start = pos
    nout, pos = read_compact(raw, pos)
    first = None
    total = 0
    for _ in range(nout):
        amount = int.from_bytes(raw[pos:pos + 8], "little")
        total += amount
        script_start = pos + 8
        script_len, pos = read_compact(raw, script_start)
        pos += script_len
        if first is None:
            first = raw[script_start:pos]
    vout_end = pos
    pos += 4  # locktime
    if int.from_bytes(raw[start:start + 4], "little") >> 16:
        payload_len, pos = read_compact(raw, pos)
        pos += payload_len
    assert first is not None
    coinbase = raw[start:vout_start] + b"\x01" + total.to_bytes(8, "little") + first + raw[vout_end:pos]
    dsha = lambda data: hashlib.sha256(hashlib.sha256(data).digest()).digest()
    layer = [dsha(coinbase)] + [bytes.fromhex(tx.get("txid", tx.get("hash")))[::-1] for tx in tpl["transactions"]]
    while len(layer) > 1:
        if len(layer) % 2:
            layer.append(layer[-1])
        layer = [dsha(layer[i] + layer[i + 1]) for i in range(0, len(layer), 2)]
    header = raw[:36] + layer[0] + raw[68:80]
    return (header + raw[80:start] + coinbase + raw[pos:]).hex()


class Recorder(http.server.HTTPServer):
    def __init__(self, port, rpc):
        self.rpc = rpc
        self.records = []
        self.negative_control = None
        self.submitted = threading.Event()
        super().__init__(("127.0.0.1", port), Handler)


class Handler(http.server.BaseHTTPRequestHandler):
    server: Recorder

    def log_message(self, *_):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert req["method"] in ("getblocktemplate", "getbestblockhash", "submitblock")
        if self.server.submitted.is_set():
            reply = {"id": req.get("id"), "result": None,
                     "error": {"code": -1, "message": "test has recorded its one submission"}}
        else:
            if req["method"] == "submitblock":
                tpl = [r["response"]["result"] for r in self.server.records
                       if r["request"]["method"] == "getblocktemplate"][-1]
                if tpl["masternode"]:
                    negative = {"id": "missing-payee", "method": "getblocktemplate", "params": [{
                        "mode": "proposal", "data": missing_payee_block(req["params"][0], tpl)}]}
                    self.server.negative_control = {"request": negative,
                                                    "response": self.server.rpc.request(negative)}
            reply = self.server.rpc.request(req)
            self.server.records.append({"request": req, "response": reply})
            if req["method"] == "submitblock":
                self.server.submitted.set()
        body = json.dumps(reply).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def mine_once(args, root, rpc, address, label, txid=None, require_mn=False):
    before = rpc("getblockcount")
    recorder = Recorder(args.rpc_port + 1, rpc)
    thread = threading.Thread(target=recorder.serve_forever, daemon=True)
    thread.start()
    log_path = root / f"miner-{label}.log"
    command = [str(args.miner), address, "--threads", "2", "--ways", str(args.ways),
               "--no-pin", "--conf", str(root / "miner.conf"),
               "--rpc", f"http://127.0.0.1:{args.rpc_port + 1}/"]
    try:
        with log_path.open("w") as log:
            proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                if not recorder.submitted.wait(args.miner_timeout):
                    raise AssertionError(f"{label}: no submitblock; see {log_path}")
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    finally:
        recorder.shutdown()
        recorder.server_close()
        thread.join()
        save(root / f"rpc-{label}.json", recorder.records)
    submits = [r for r in recorder.records if r["request"]["method"] == "submitblock"]
    assert len(submits) == 1, submits
    assert submits[0]["response"]["error"] is None, submits[0]["response"]
    assert submits[0]["response"]["result"] is None, submits[0]["response"]
    assert rpc("getblockcount") == before + 1
    block_hash = rpc("getblockhash", before + 1)
    block = rpc("getblock", block_hash, 2)
    raw = submits[0]["request"]["params"][0]
    assert rpc("getblock", block_hash, 0) == raw
    tpl = [r["response"]["result"] for r in recorder.records
           if r["request"]["method"] == "getblocktemplate"][-1]
    assert tpl["height"] == block["height"] and tpl["curtime"] == block["time"]
    cb = block["tx"][0]
    outs = [{"amount": o["valueSat"], "script": o["scriptPubKey"]["hex"]} for o in cb["vout"]]
    required = tpl["masternode"] + tpl["superblock"]
    if require_mn:
        assert len(tpl["masternode"]) == 2, "Expected real owner and operator masternode payouts"
        save(root / f"negative-{label}.json", recorder.negative_control)
        assert recorder.negative_control["response"]["result"] == "bad-cb-payee", recorder.negative_control
    assert outs[1:] == [{"amount": o["amount"], "script": o["script"]} for o in required]
    assert outs[0]["amount"] == tpl["coinbasevalue"] - sum(o["amount"] for o in required)
    assert sum(o["amount"] for o in outs) == tpl["coinbasevalue"]
    assert outs[0]["script"] == rpc("validateaddress", address)["scriptPubKey"]
    assert cb.get("extraPayload", "") == tpl.get("coinbase_payload", "")
    if txid:
        assert txid in [tx["txid"] for tx in block["tx"]]
        assert txid not in rpc("getrawmempool")
    receipt = {"label": label, "hash": block_hash, "height": block["height"],
               "time": block["time"], "bits": block["bits"], "header": raw[:160],
               "txid": txid, "coinbase_txid": cb["txid"], "outputs": outs,
               "coinbasevalue": tpl["coinbasevalue"], "masternode": tpl["masternode"],
               "superblock": tpl["superblock"], "coinbase_payload": tpl.get("coinbase_payload"),
               "template_fees": sum(tx["fee"] for tx in tpl["transactions"]),
               "submitblock": None, "chain_readback_exact": True, "command": command,
               "missing_payee_rejection": recorder.negative_control["response"]["result"] if require_mn else None}
    save(root / f"block-{label}.json", receipt)
    print(json.dumps({k: receipt[k] for k in ("label", "height", "hash", "time")}), flush=True)
    return receipt


def verify_pow(root, blocks):
    libpath = root / "reference.so"
    command = [os.environ.get("CC", "cc"), "-O2", "-shared", "-fPIC",
               str(REPO / "src/crypto/yespower/yespower.c"), "-o", str(libpath)]
    built = subprocess.run(command, capture_output=True, text=True, check=True)
    (root / "reference-build.log").write_text(built.stdout + built.stderr)
    lib = ctypes.CDLL(str(libpath))
    lib.yespower_set_v050_fork_time.argtypes = [ctypes.c_uint32]
    lib.yespower_hash.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    results = []
    for b in blocks:
        raw = bytes.fromhex(b["header"])
        hashes = {}
        for label, fork in (("fork_armed", FORK), ("legacy_forced", 0), ("v050_forced", 1)):
            lib.yespower_set_v050_fork_time(fork)
            out = ctypes.create_string_buffer(32)
            assert lib.yespower_hash(raw, out) == 0
            hashes[label] = out.raw[::-1].hex()
        assert hashes["fork_armed"] == b["hash"]
        expected = "v050_forced" if b["time"] >= FORK else "legacy_forced"
        other = "legacy_forced" if b["time"] >= FORK else "v050_forced"
        assert hashes[expected] == b["hash"] and hashes[other] != b["hash"]
        bits = int(b["bits"], 16)
        target = (bits & 0x7fffff) << (8 * ((bits >> 24) - 3))
        assert int(b["hash"], 16) <= target
        results.append({"height": b["height"], "expected_params": "256/8" if b["time"] >= FORK else "2048/32",
                        "target": f"{target:064x}", "hashes": hashes})
    return results


def run(args, root, receipt):
    password = os.urandom(24).hex()
    config = root / "miner.conf"
    config.write_text(f"rpcuser=miner-test\nrpcpassword={password}\n")
    config.chmod(0o600)
    data = root / "node"
    data.mkdir()
    rpc = RPC(args.rpc_port, password)
    wallet = RPC(args.rpc_port, password, "faucet")
    receiver = RPC(args.rpc_port, password, "receiver")
    command = [str(args.daemon), "-regtest", f"-datadir={data}", f"-conf={config}",
               f"-rpcport={args.rpc_port}", "-rpcbind=127.0.0.1", "-rpcallowip=127.0.0.1",
               "-listen=0", "-connect=0", "-dnsseed=0", "-fixedseeds=0", "-discover=0",
               "-listenonion=0", "-server=1", "-par=1", "-parbls=1", "-rpcthreads=1",
               "-dbcache=64", "-maxmempool=32", "-keypool=20", "-txindex=1",
               "-fallbackfee=0.0001", "-dip3params=2:2", f"-mocktime={FORK - 1000}",
               f"-testactivationheight=smt050pow@{FORK}", f"-paramsdir={args.paramsdir}"]
    receipt["node_command"] = command
    receipt["miner_threads"] = 2
    receipt["fork_time"] = FORK
    receipt["daemon_sha256"] = hashlib.sha256(args.daemon.read_bytes()).hexdigest()
    receipt["miner_sha256"] = hashlib.sha256(args.miner.read_bytes()).hexdigest()
    with (root / "node.log").open("w") as log:
        node = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: rpc("getblockchaininfo")["chain"] == "regtest")
            receipt["node_version"] = rpc("getnetworkinfo")["subversion"]
            assert rpc("getblockcount") == 0
            rpc("createwallet", "faucet")
            rpc("createwallet", "receiver")
            address = wallet("getnewaddress")
            blocks = receipt["blocks"] = []
            blocks.append(mine_once(args, root, rpc, address, "pre-no-mn"))
            # Mature enough real coinbases to fund the chain's 15,000 SMT collateral.
            rpc("generatetoaddress", 145, address)
            receipt["funding_balance"] = wallet("getbalance")
            assert receipt["funding_balance"] > 15000
            collateral = wallet("getnewaddress")
            owner = wallet("getnewaddress")
            payout = wallet("getnewaddress")
            operator = rpc("bls", "generate")
            protx = wallet("protx", "register_fund", collateral, "127.0.0.1:29189", owner,
                           operator["public"], owner, 10, payout, address)
            rpc("generatetoaddress", 2, address)
            operator_payout = wallet("getnewaddress")
            wallet("protx", "update_service", protx, "127.0.0.1:29189", operator["secret"],
                   operator_payout, address)
            rpc("generatetoaddress", 1, address)
            receipt["protx_hash"] = protx
            receipt["protx_info"] = rpc("protx", "info", protx)
            recipient = receiver("getnewaddress")
            for label, timestamp in (("pre-mn-transfer", FORK - 1), ("at-fork-mn-transfer", FORK),
                                     ("post-mn-transfer", FORK + 1)):
                rpc("setmocktime", timestamp)
                txid = wallet("sendtoaddress", recipient, 1.25)
                assert txid in rpc("getrawmempool")
                blocks.append(mine_once(args, root, rpc, address, label, txid, require_mn=True))
                assert receiver("gettransaction", txid)["confirmations"] >= 1
            receipt["pow_checks"] = verify_pow(root, blocks)
            rpc("setmocktime", FORK + 100)
            rpc("generatetoaddress", 101, address)
            receipt["mature_coinbases"] = [wallet("gettransaction", b["coinbase_txid"]) for b in blocks]
            for tx in receipt["mature_coinbases"]:
                assert tx["confirmations"] >= 101
                assert any(d["category"] == "generate" for d in tx["details"])
            receipt["receiver_balance"] = receiver("getbalance")
            assert receipt["receiver_balance"] == 3.75
            receipt["verifychain"] = rpc("verifychain", 4, 0)
            assert receipt["verifychain"] is True
            receipt["tip"] = rpc("getbestblockhash")
            receipt["height"] = rpc("getblockcount")
            receipt["integration"] = "PASS"
        finally:
            try:
                rpc("stop")
            except (OSError, RuntimeError):
                node.terminate()
            try:
                node.wait(timeout=30)
            except subprocess.TimeoutExpired:
                node.kill()
                node.wait()
            receipt["node_exit_code"] = node.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--miner", type=Path, default=HERE / "smt-miner")
    parser.add_argument("--paramsdir", type=Path, default=REPO / "params")
    parser.add_argument("--scratch", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--rpc-port", type=int, default=29182)
    parser.add_argument("--ways", type=int, choices=(1, 2), default=1)
    parser.add_argument("--miner-timeout", type=int, default=20)
    args = parser.parse_args()
    assert 29000 <= args.rpc_port <= 29990, "Use dedicated 29xxx test ports"
    for port in (args.rpc_port, args.rpc_port + 1):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", port))
    args.daemon = args.daemon.resolve(strict=True)
    args.miner = args.miner.resolve(strict=True)
    root = Path(tempfile.mkdtemp(prefix="smt-miner-integration-", dir=args.scratch))
    print(f"receipts: {root}", flush=True)
    receipt = {"integration": "FAIL", "receipt_dir": str(root)}
    try:
        run(args, root, receipt)
    except Exception as exc:
        receipt["error"] = str(exc)
        raise
    finally:
        save(root / "receipt.json", receipt)
        print(json.dumps({k: receipt[k] for k in ("integration", "receipt_dir", "error") if k in receipt}), flush=True)


if __name__ == "__main__":
    main()
