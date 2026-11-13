#!/usr/bin/env python3
"""Minimal reader for btmon -w (BTSnoop monitor format) files: list ATT PDUs per ACL handle.
`btmon -r` crashed on some long captures; this reader is the fallback.
Usage: python3 snoop_att.py FILE [--since SECONDS] [--summary]
"""
import struct
import sys
from collections import Counter

OPS = {0x01: "ErrorRsp", 0x02: "MTUReq", 0x03: "MTURsp", 0x04: "FindInfoReq", 0x05: "FindInfoRsp",
       0x08: "ReadByTypeReq", 0x09: "ReadByTypeRsp", 0x0a: "ReadReq", 0x0b: "ReadRsp", 0x10: "ReadByGroupReq",
       0x11: "ReadByGroupRsp", 0x12: "WriteReq", 0x13: "WriteRsp", 0x1b: "Notify", 0x1d: "Indicate",
       0x1e: "Confirm", 0x52: "WriteCmd"}
EPOCH_DELTA = 0x00dcddb30f2f8000  # btsnoop µs epoch (year 0) → 1970


def records(path):
    with open(path, "rb") as f:
        f.read(16)
        while True:
            hdr = f.read(24)
            if len(hdr) < 24:
                return
            orig, incl, flags, drops, ts = struct.unpack(">IIIIq", hdr)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield (ts - EPOCH_DELTA) / 1e6, flags & 0xffff, data  # monitor format: opcode in low 16 bits


def main():
    path = sys.argv[1]
    since = float(sys.argv[sys.argv.index("--since") + 1]) if "--since" in sys.argv else 0
    summary = "--summary" in sys.argv
    t0 = None
    counts = Counter()
    for t, op, data in records(path):
        t0 = t0 or t
        rel = t - t0
        if rel < since or op not in (4, 5) or len(data) < 9:  # 4 = ACL TX, 5 = ACL RX
            continue
        handle = struct.unpack("<H", data[0:2])[0] & 0x0fff
        l2len, cid = struct.unpack("<HH", data[4:8])
        if cid != 4:
            continue
        pdu = data[8:8 + l2len]
        d = "TX" if op == 4 else "RX"
        name = OPS.get(pdu[0], f"op{pdu[0]:02x}")
        if summary:
            counts[(handle, d, name)] += 1
        else:
            att_h = struct.unpack("<H", pdu[1:3])[0] if len(pdu) >= 3 else 0
            print(f"{rel:9.3f} {handle} {d} {name:14} h=0x{att_h:04x} {pdu[3:].hex()}")
    for k, v in sorted(counts.items()):
        print(v, *k)


if __name__ == "__main__":
    main()
