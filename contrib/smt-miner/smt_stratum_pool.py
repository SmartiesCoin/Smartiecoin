#!/usr/bin/env python3
"""Minimal Stratum v1 reference pool for a Smartiecoin node (testing / development tool).

It is NOT a production pool: no accounting, no payouts, no authentication, no DoS protection.
It exists to exercise Stratum clients end to end and to document the protocol conventions:

  * job source ........ the node's getblocktemplate
  * share validation .. the node's own yespower_hash() (via libsmt.so), not a re-implementation
  * block submission .. submitblock, when a share also meets the network target
  * difficulty-1 ...... 0x0000ffff00..00 ("scrypt" convention); share target = diff1 / difficulty
  * prevhash .......... sent with every 4-byte word byte-swapped (standard Stratum v1)
  * ntime / nonce ..... sent and received as %08x of the numeric header field

    ./smt_stratum_pool.py <pool payout address> [--listen 127.0.0.1:3333] [--ease 16]

--ease N makes shares N times easier than a block (so a test miner sees frequent shares).
"""
import argparse
import base64
import ctypes
import hashlib
import json
import os
import socketserver
import struct
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
DIFF1 = 0xFFFF << 224
MAX256 = (1 << 256) - 1
# consensus.nSMTv050PowTime (same value smt-miner.c uses): per-header yespower parameter switch.
SMT_V050_FORK_TIME = 1790528400


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def varint(n):
    return bytes([n]) if n < 0xFD else b"\xfd" + struct.pack("<H", n)


def push(b):
    return bytes([len(b)]) + b


def swap4(b):
    return b"".join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def b58check(addr):
    n = 0
    for c in addr:
        n = n * 58 + B58.index(c)
    raw = n.to_bytes(25, "big")
    assert dsha(raw[:-4])[:4] == raw[-4:], "bad address checksum"
    return raw[1:-4]


def height_script(h):
    if 1 <= h <= 16:
        return bytes([0x50 + h])
    return push(h.to_bytes((h.bit_length() + 8) // 8, "little"))


def merkle_steps(txids):
    """Sibling hashes for the coinbase (index 0), as sent in mining.notify."""
    layer = [None] + list(txids)
    steps = []
    while len(layer) > 1:
        steps.append(layer[1])
        if len(layer) % 2:
            layer.append(layer[-1])
        layer = [None] + [dsha(layer[i] + layer[i + 1]) for i in range(2, len(layer), 2)]
    return steps


class Node:
    def __init__(self, conf_path):
        conf = {}
        for line in open(conf_path):
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                conf[k.strip()] = v.strip()
        self.url = "http://127.0.0.1:%s/" % conf.get("rpcport", "8282")
        self.auth = "Basic " + base64.b64encode((conf["rpcuser"] + ":" + conf["rpcpassword"]).encode()).decode()

    def call(self, method, *params):
        req = urllib.request.Request(self.url, json.dumps({"method": method, "params": list(params), "id": 1}).encode(),
                                     {"Authorization": self.auth, "Content-Type": "application/json"})
        try:
            r = json.load(urllib.request.urlopen(req, timeout=60))
        except urllib.error.HTTPError as e:
            r = json.load(e)
        if r.get("error"):
            raise RuntimeError("%s: %s" % (method, r["error"]))
        return r["result"]


class Pool:
    def __init__(self, node, address, ease):
        self.node, self.spk, self.ease = node, b"\x76\xa9\x14" + b58check(address) + b"\x88\xac", ease
        lib = ctypes.CDLL(os.path.join(HERE, "libsmt.so"))
        # The node's yespower.c dispatches per header nTime once the fork time is armed; without this the
        # pool would validate post-fork shares with the old parameters. Fail loudly on an old library.
        lib.yespower_set_v050_fork_time.argtypes = [ctypes.c_uint32]
        lib.yespower_set_v050_fork_time(SMT_V050_FORK_TIME)
        lib.yespower_hash.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib = lib
        self.lock = threading.Lock()
        self.jobs = {}
        self.current = None
        self.clients = []
        self.seq = 0
        self.stats = {"accepted": 0, "rejected": 0, "blocks": 0}
        self.seen = set()

    def hash(self, header):
        out = ctypes.create_string_buffer(32)
        self.lib.yespower_hash(header, out)
        return int.from_bytes(out.raw, "little")

    def make_job(self):
        t = self.node.call("getblocktemplate", {"rules": ["segwit"]})
        if t.get("coinbase_payload"):
            raise RuntimeError("template needs a coinbase payload; not supported by this reference pool")
        hs = height_script(t["height"])
        tag = push(b"smt-pool")
        sig_len = len(hs) + 1 + 8 + len(tag)          # 8 = extranonce1 (4) + extranonce2 (4)
        coinb1 = struct.pack("<I", 1) + b"\x01" + b"\x00" * 32 + b"\xff\xff\xff\xff" + varint(sig_len) + hs + b"\x08"
        coinb2 = (tag + b"\xff\xff\xff\xff" + b"\x01" + struct.pack("<q", t["coinbasevalue"]) + varint(len(self.spk)) +
                  self.spk + struct.pack("<I", 0))
        txids = [bytes.fromhex(x.get("txid") or x["hash"])[::-1] for x in t["transactions"]]
        with self.lock:
            self.seq += 1
            job = {
                "id": "%x" % self.seq, "height": t["height"], "prev": bytes.fromhex(t["previousblockhash"])[::-1],
                "coinb1": coinb1, "coinb2": coinb2, "steps": merkle_steps(txids), "version": t["version"],
                "bits": int(t["bits"], 16), "ntime": t["curtime"], "target": int(t["target"], 16),
                "txdata": [bytes.fromhex(x["data"]) for x in t["transactions"]], "born": time.time(),
            }
            self.jobs[job["id"]] = job
            for old in list(self.jobs)[:-8]:
                del self.jobs[old]
            self.current = job
        return job

    def share_target(self, job):
        return min(job["target"] * self.ease, MAX256)

    def difficulty(self, job):
        return DIFF1 / self.share_target(job)

    def notify_msgs(self, job):
        return [
            {"id": None, "method": "mining.set_difficulty", "params": [self.difficulty(job)]},
            {"id": None, "method": "mining.notify", "params": [
                job["id"], swap4(job["prev"]).hex(), job["coinb1"].hex(), job["coinb2"].hex(),
                [s.hex() for s in job["steps"]], "%08x" % job["version"], "%08x" % job["bits"],
                "%08x" % job["ntime"], True]},
        ]

    def broadcast(self, job):
        with self.lock:
            clients = list(self.clients)
        for c in clients:
            try:
                for m in self.notify_msgs(job):
                    c.send(m)
            except OSError:
                pass

    def refresher(self):
        last_tip = None
        while True:
            try:
                tip = self.node.call("getbestblockhash")
                cur = self.current
                if cur is None or tip != last_tip or time.time() - cur["born"] > 30:
                    last_tip = tip
                    self.broadcast(self.make_job())
            except Exception as e:                      # node down / no peers: keep trying
                print("job refresh error:", e, flush=True)
            time.sleep(2)

    def submit(self, params, en1):
        user, job_id, en2hex, ntimehex, noncehex = params
        with self.lock:
            job = self.jobs.get(job_id)
        if job is None:
            return False, [21, "stale job", None]
        key = (job_id, en2hex, ntimehex, noncehex, en1.hex())
        if key in self.seen:
            return False, [22, "duplicate share", None]
        self.seen.add(key)
        cb = job["coinb1"] + en1 + bytes.fromhex(en2hex) + job["coinb2"]
        root = dsha(cb)
        for s in job["steps"]:
            root = dsha(root + s)
        header = (struct.pack("<I", job["version"]) + job["prev"] + root + struct.pack("<I", int(ntimehex, 16)) +
                  struct.pack("<I", job["bits"]) + struct.pack("<I", int(noncehex, 16)))
        value = self.hash(header)
        if value > self.share_target(job):
            return False, [23, "low difficulty share", None]
        if value <= job["target"]:
            block = header + varint(1 + len(job["txdata"])) + cb + b"".join(job["txdata"])
            try:
                res = self.node.call("submitblock", block.hex())
            except RuntimeError as e:
                res = str(e)
            print("BLOCK height=%d submitblock=%s" % (job["height"], "accepted" if res is None else res), flush=True)
            if res is None:
                self.stats["blocks"] += 1
        return True, None


class Handler(socketserver.StreamRequestHandler):
    def send(self, obj):
        self.wfile.write((json.dumps(obj) + "\n").encode())
        self.wfile.flush()

    def handle(self):
        pool = self.server.pool
        en1 = os.urandom(4)
        subscribed = False
        print("client connected:", self.client_address, flush=True)
        try:
            for raw in self.rfile:
                msg = json.loads(raw)
                m, p, i = msg.get("method"), msg.get("params", []), msg.get("id")
                if m == "mining.subscribe":
                    self.send({"id": i, "result": [[["mining.set_difficulty", "1"], ["mining.notify", "1"]], en1.hex(), 4],
                               "error": None})
                    subscribed = True
                elif m == "mining.authorize":
                    self.send({"id": i, "result": True, "error": None})
                    with pool.lock:
                        pool.clients.append(self)
                    job = pool.current or pool.make_job()
                    for x in pool.notify_msgs(job):
                        self.send(x)
                elif m == "mining.submit" and subscribed:
                    ok, err = pool.submit(p, en1)
                    pool.stats["accepted" if ok else "rejected"] += 1
                    self.send({"id": i, "result": ok, "error": err})
                    if not ok:
                        print("rejected share:", err[1], flush=True)
                elif i is not None:
                    self.send({"id": i, "result": None, "error": [20, "unknown method", None]})
        except (OSError, ValueError):
            pass
        finally:
            with pool.lock:
                if self in pool.clients:
                    pool.clients.remove(self)
            print("client disconnected; stats:", pool.stats, flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("address", help="address that receives block rewards")
    ap.add_argument("--listen", default="127.0.0.1:3333")
    ap.add_argument("--conf", default=None)
    ap.add_argument("--ease", type=int, default=16, help="share target = block target * ease")
    a = ap.parse_args()
    conf = a.conf or next(p for p in map(os.path.expanduser, ["~/.smartiecoin/smartiecoin.conf",
                                                             "~/.smartiecoincore/smartiecoin.conf"]) if os.path.exists(p))
    pool = Pool(Node(conf), a.address, a.ease)
    host, port = a.listen.rsplit(":", 1)
    srv = Server((host, int(port)), Handler)
    srv.pool = pool
    threading.Thread(target=pool.refresher, daemon=True).start()
    print("reference pool listening on %s (share target = block target x %d)" % (a.listen, a.ease), flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
