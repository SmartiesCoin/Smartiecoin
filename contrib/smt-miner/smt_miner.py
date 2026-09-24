#!/usr/bin/env python3
"""Smartiecoin solo CPU miner (Yespower) for the Smartiecoin node's getblocktemplate/submitblock RPC.

The hash function is the node's own yespower_hash() (built by build.sh into libsmt.so), so a
nonce found here is by construction valid for the node's consensus code.

    ./smt_miner.py <payout address> [--threads N] [--conf PATH] [--rpc URL]
"""
import argparse
import base64
import ctypes
import hashlib
import json
import os
import signal
import struct
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
CHUNK = 4096            # nonces per native call (bounds latency of stop/new-block detection)
TEMPLATE_MAX_AGE = 30   # seconds before the template is refreshed (new time / new mempool txs)
# consensus.nSMTv050PowTime: a header whose nTime reaches this uses the v0.5.0 (N=256, r=8) yespower
# parameters; the native yespower_hash() dispatches per header once this is armed.
SMT_V050_FORK_TIME = 1790528400


def find_conf(explicit):
    if explicit:
        return explicit
    for d in ("~/.smartiecoin/smartiecoin.conf", "~/.smartiecoincore/smartiecoin.conf"):
        if os.path.exists(os.path.expanduser(d)):
            return os.path.expanduser(d)
    sys.exit("no smartiecoin.conf found; pass --conf")


def read_conf(path):
    out = {}
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


class Rpc:
    def __init__(self, url, user, password):
        self.url = url
        self.auth = "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode()

    def call(self, method, *params):
        body = json.dumps({"method": method, "params": list(params), "id": 1}).encode()
        req = urllib.request.Request(self.url, body, {"Authorization": self.auth, "Content-Type": "application/json"})
        try:
            r = json.load(urllib.request.urlopen(req, timeout=60))
        except urllib.error.HTTPError as e:      # bitcoind returns HTTP 500 with a JSON error body
            r = json.load(e)
        if r.get("error"):
            raise RuntimeError("%s: %s" % (method, r["error"]))
        return r["result"]


def b58check_decode(addr):
    n = 0
    for c in addr:
        n = n * 58 + B58.index(c)
    raw = n.to_bytes(25, "big")
    if hashlib.sha256(hashlib.sha256(raw[:-4]).digest()).digest()[:4] != raw[-4:]:
        raise ValueError("bad address checksum")
    return raw[0], raw[1:-4]


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def varint(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xffffffff:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


def push(b):
    return bytes([len(b)]) + b


def height_script(h):
    if 1 <= h <= 16:
        return bytes([0x50 + h])
    return push(h.to_bytes((h.bit_length() + 8) // 8, "little"))


def merkle_root(txids):
    layer = list(txids)
    while len(layer) > 1:
        if len(layer) % 2:
            layer.append(layer[-1])
        layer = [dsha(layer[i] + layer[i + 1]) for i in range(0, len(layer), 2)]
    return layer[0]


def build_block_parts(t, spk, extranonce):
    """Return (80-byte header with zero nonce, serialized tx section incl. count)."""
    for key in ("masternode", "superblock"):
        if t.get(key):
            raise RuntimeError("template requires %s payments; not supported by this miner" % key)
    sig = height_script(t["height"]) + push(struct.pack("<Q", extranonce)) + push(b"smartiecoin")
    payload = bytes.fromhex(t.get("coinbase_payload", ""))
    version = 3 | (5 << 16) if payload else 1        # DIP3 special tx (coinbase) only if the node asks for it
    cb = (struct.pack("<I", version) + b"\x01" + b"\x00" * 32 + b"\xff\xff\xff\xff" + varint(len(sig)) + sig +
          b"\xff\xff\xff\xff" + b"\x01" + struct.pack("<q", t["coinbasevalue"]) + varint(len(spk)) + spk +
          struct.pack("<I", 0))
    if payload:
        cb += varint(len(payload)) + payload
    txids = [dsha(cb)]
    body = cb
    for tx in t["transactions"]:
        txids.append(bytes.fromhex(tx.get("txid") or tx["hash"])[::-1])
        body += bytes.fromhex(tx["data"])
    header = (struct.pack("<I", t["version"]) + bytes.fromhex(t["previousblockhash"])[::-1] + merkle_root(txids) +
              struct.pack("<II", t["curtime"], int(t["bits"], 16)) + b"\x00\x00\x00\x00")
    return header, varint(len(txids)) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("address", help="payout address (P2PKH)")
    ap.add_argument("--threads", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--conf", help="path to smartiecoin.conf (default: ~/.smartiecoin/smartiecoin.conf)")
    ap.add_argument("--rpc", help="RPC URL override, e.g. http://127.0.0.1:8282/")
    ap.add_argument("--lib", default=os.path.join(HERE, "libsmt.so"))
    args = ap.parse_args()

    conf = read_conf(find_conf(args.conf))
    rpc = Rpc(args.rpc or "http://127.0.0.1:%s/" % conf.get("rpcport", "8282"), conf["rpcuser"], conf["rpcpassword"])
    _, h160 = b58check_decode(args.address)
    spk = b"\x76\xa9\x14" + h160 + b"\x88\xac"

    lib = ctypes.CDLL(args.lib)
    # libsmt.so is built from the node's own yespower.c. Without this call the library would hash with
    # the pre-fork parameters even for post-fork headers; fail loudly on an old library.
    lib.yespower_set_v050_fork_time.argtypes = [ctypes.c_uint32]
    lib.yespower_set_v050_fork_time(SMT_V050_FORK_TIME)
    lib.scan.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_uint32)]
    lib.scan.restype = ctypes.c_int

    stop = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.append(1))

    extranonce = int(time.time()) << 8
    hashes = blocks = 0
    t0 = time.time()
    print("smt-miner: %d threads -> %s" % (args.threads, args.address), flush=True)

    while not stop:
        try:
            t = rpc.call("getblocktemplate", {"rules": ["segwit"]})
            extranonce += 1
            header, txs = build_block_parts(t, spk, extranonce)
        except RuntimeError as e:
            print("template error: %s (retrying)" % e, flush=True)
            time.sleep(5)
            continue
        except (urllib.error.URLError, OSError) as e:
            print("node unreachable: %s (retrying)" % e, flush=True)
            time.sleep(5)
            continue

        target = bytes.fromhex(t["target"])[::-1]
        nonce = ctypes.c_uint32(0)
        start, born, last_check = 0, time.time(), time.time()
        while not stop and start < 0xffffffff:
            if lib.scan(header, target, start, CHUNK, args.threads, ctypes.byref(nonce)):
                hashes += CHUNK
                block = header[:76] + struct.pack("<I", nonce.value) + txs
                try:
                    res = rpc.call("submitblock", block.hex())
                except RuntimeError as e:
                    res = "error: %s" % e
                if res is None:
                    blocks += 1
                print("%s height=%d nonce=%d submitblock=%s" % (time.strftime("%H:%M:%S"), t["height"], nonce.value,
                                                             "accepted" if res is None else res), flush=True)
                break
            hashes += CHUNK
            start += CHUNK
            now = time.time()
            if now - born > TEMPLATE_MAX_AGE:
                break
            if now - last_check > 2:           # cheap tip check so we never mine on a stale tip
                last_check = now
                try:
                    if rpc.call("getbestblockhash") != t["previousblockhash"]:
                        break
                except Exception:
                    break
        el = time.time() - t0
        print("%s rate=%.0f H/s blocks=%d" % (time.strftime("%H:%M:%S"), hashes / max(el, 1e-9), blocks), flush=True)
    print("stopped; blocks found: %d" % blocks)


if __name__ == "__main__":
    main()
