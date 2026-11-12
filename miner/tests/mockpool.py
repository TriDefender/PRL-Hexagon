#!/usr/bin/env python3
"""Minimal Pearl stratum mock pool for end-to-end testing of prl-miner.

Sends a mining.notify with a synthetic header + easy target, captures
mining.submit shares to a file, and replies success. Optionally speaks the
kryptex dialect (jsonrpc frames + subscribe-first)."""
import json
import socket
import struct
import sys
import threading

MODE = sys.argv[1] if len(sys.argv) > 1 else "object"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3344
OUT = sys.argv[3] if len(sys.argv) > 3 else "/tmp/mock_share.b64"

HEADER = bytes.fromhex(
    "00000020" + "03" * 32 + "04" * 32 + "66666666" + "ffff7f1d")
TARGET_BE = bytes.fromhex(
    "0000100000000000000000000000000000000000000000000000000000000000")

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(1)
print(f"mock pool ({MODE}) on {PORT}", flush=True)

conn, addr = srv.accept()
print("miner connected", flush=True)
f = conn.makefile("rwb")
share_count = 0

def notify():
    params = {
        "job_id": "deadbeef_1",
        "header": HEADER.hex(),
        "target": TARGET_BE.hex(),
        "height": 12345,
        "cert_version": 3,
        "diff": 1024,
    }
    msg = {"id": None, "method": "mining.notify", "params": params}
    if MODE == "kryptex":
        msg["jsonrpc"] = "2.0"
    f.write((json.dumps(msg) + "\n").encode())
    f.flush()

notify()

for line in f:
    req = json.loads(line)
    method = req.get("method")
    rid = req.get("id")
    print(f"<- {method} (id={rid})", flush=True)
    if method == "mining.authorize":
        f.write((json.dumps({"id": rid, "error": None, "result": True}) + "\n").encode())
        f.flush()
        notify()
    elif method == "mining.submit":
        share_count += 1
        proof = req["params"]["plain_proof"]
        with open(OUT, "w") as out:
            out.write(proof)
        f.write((json.dumps({"id": rid, "error": None, "result": True}) + "\n").encode())
        f.flush()
        print(f"got share #{share_count} ({len(proof)} b64 chars), job_id={req['params']['job_id']}",
              flush=True)
        break

# keep the connection open briefly so the miner's submit doesn't race
import time
time.sleep(1)
print("mock pool done", flush=True)
