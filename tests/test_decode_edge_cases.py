#!/usr/bin/env python3
"""
Edge cases for both decoders (ctrace-decode and ctrace_decode.py):
- truncated records, unknown event IDs, trailing garbage, corrupt zstd (non-zero exit, no hang)
- empty traces (exit code 0)
- output ordering and timestamp formatting with and without a clock anchor
- SQLite output: types, value ranges, quoted identifiers
"""

import sys
import os
import subprocess
import tempfile
import struct
import sqlite3
import re
import json

def check_decoder(decoder, schema_path, tmpdir, label):
    """Runs every check with one decoder command (list of argv prefix)."""
    def run(*args, **kw):
        return subprocess.run(list(decoder) + [schema_path] + list(args), capture_output=True, text=True, timeout=20, **kw)

    # 1. Empty trace file -> exit code 0
    empty_bin = os.path.join(tmpdir, "empty.bin")
    open(empty_bin, "wb").close()
    res = run(empty_bin)
    assert res.returncode == 0, f"[{label}] expected 0 for empty trace, got {res.returncode}"

    # 2. Unknown event ID -> non-zero exit code
    unknown_bin = os.path.join(tmpdir, "unknown.bin")
    with open(unknown_bin, "wb") as f:
        f.write(struct.pack("<HQ", 9999, 12345678))
    res = run(unknown_bin)
    assert res.returncode != 0, f"[{label}] expected non-zero exit code for unknown event ID"

    # 3. Truncated record (event 2 with only 4 payload bytes)
    trunc_bin = os.path.join(tmpdir, "truncated.bin")
    with open(trunc_bin, "wb") as f:
        f.write(struct.pack("<HQ", 2, 12345678) + b"\x01\x02\x03\x04")
    res = run(trunc_bin)
    assert res.returncode != 0, f"[{label}] expected non-zero exit code for truncated record"

    # 4. Trailing garbage bytes
    payload = b"testpayload"
    rec = lambda ts, fd, pl=payload: struct.pack("<HQ", 2, ts) + struct.pack("<i", fd) + struct.pack("<Q", 64) + struct.pack("<I", len(pl)) + pl
    valid_rec = rec(1000000, 42)
    garbage_bin = os.path.join(tmpdir, "garbage.bin")
    with open(garbage_bin, "wb") as f:
        f.write(valid_rec + b"\xaa\xbb\xcc")
    res = run(garbage_bin)
    assert res.returncode != 0, f"[{label}] expected non-zero exit code for trailing unparsed bytes"

    # 5. Truncated CountedArray payload (event 1 has counted array `sensors`): blen claims 64 bytes
    trunc_ca_bin = os.path.join(tmpdir, "trunc_ca.bin")
    with open(trunc_ca_bin, "wb") as f:
        f.write(struct.pack("<HQ", 1, 1000) + struct.pack("<iH", 101, 2) + struct.pack("<I", 64) + b"\x01\x02\x03\x04")
    res = run(trunc_ca_bin)
    assert res.returncode != 0, f"[{label}] expected non-zero exit code for truncated CountedArray"

    # 6. Events are written out ordered by timestamp, whatever their order in the file
    unordered_bin = os.path.join(tmpdir, "unordered.bin")
    with open(unordered_bin, "wb") as f:
        f.write(rec(3000, 3, b"") + rec(1000, 1, b"") + rec(2000, 2, b""))
    res = run(unordered_bin, "--ns")
    assert res.returncode == 0, res.stderr
    assert [l.split("]")[0] for l in res.stdout.splitlines()] == ["[1000", "[2000", "[3000"], res.stdout

    # 7. Without a clock anchor timestamps are printed as seconds; with one, as local dates
    res = run(unordered_bin)
    assert res.stdout.startswith("[0.000001000] "), res.stdout
    anchor = struct.pack("<HQQ", 0, 5000, 1_700_000_000_123_456_789)  # trace_ns=5000 <-> 2023-11-14T22:13:20.123456789Z
    anchored_bin = os.path.join(tmpdir, "anchored.bin")
    with open(anchored_bin, "wb") as f:
        f.write(anchor + rec(6000, 1))  # event 1000 ns after the anchor
    res = run(anchored_bin)
    assert res.returncode == 0, res.stderr
    assert re.match(r"^\[20\d\d-\d\d-\d\d \d\d:\d\d:\d\d\.123457789\] net_rx ", res.stdout), res.stdout

    # 8. An anchor alone is a valid, empty trace
    anchor_only = os.path.join(tmpdir, "anchor_only.bin")
    with open(anchor_only, "wb") as f:
        f.write(anchor)
    res = run(anchor_only)
    assert res.returncode == 0 and res.stdout == "", (res.returncode, res.stdout)

    # 9. SQLite output: one table per event, scalars as numbers, bytes as BLOB, -q silences text.
    #    uint64 above INT64_MAX wraps (SQLite has no unsigned 64-bit), an empty payload is an empty BLOB.
    db_path = os.path.join(tmpdir, "out.db")
    valid_bin = os.path.join(tmpdir, "valid.bin")
    big_len = struct.pack("<HQ", 2, 2000000) + struct.pack("<i", -5) + struct.pack("<Q", 2**64 - 1) + struct.pack("<I", 0)
    with open(valid_bin, "wb") as f:
        f.write(anchor + valid_rec + big_len)
    res = run(valid_bin, "--sqlite", db_path, "-q")
    assert res.returncode == 0 and res.stdout == "", (label, res.returncode, res.stdout, res.stderr)
    db = sqlite3.connect(db_path)
    rows = db.execute("SELECT ts_ns, ts, arg_0, len, payload, typeof(payload) FROM ctrace_net_rx ORDER BY ts_ns;").fetchall()
    db.close()
    assert rows[0][0] == 1000000 and rows[0][2] == 42 and rows[0][3] == 64 and rows[0][4] == payload, rows
    assert re.match(r"^20\d\d-\d\d-\d\d \d\d:\d\d:\d\d\.\d{9}$", rows[0][1]), rows
    assert rows[1][2] == -5 and rows[1][3] == -1 and rows[1][4] == b"" and rows[1][5] == "blob", rows

    # 10. SQL keywords as event and field names are quoted
    with open(schema_path) as f:
        schema = json.load(f)
    ev = next(e for e in schema["events"] if e["name"] == "net_rx")
    ev["name"] = "select"
    ev["fields"][0]["name"] = "order"
    ev["fields"][1]["name"] = "group"
    schema["events"] = [ev]
    kw_schema = os.path.join(tmpdir, "kw_schema.json")
    with open(kw_schema, "w") as f:
        json.dump(schema, f)
    kw_db = os.path.join(tmpdir, "kw.db")
    res = subprocess.run(list(decoder) + [kw_schema, anchored_bin, "--sqlite", kw_db, "-q"], capture_output=True, text=True, timeout=20)
    assert res.returncode == 0, (label, res.stderr)
    db = sqlite3.connect(kw_db)
    assert db.execute('SELECT "order", "group" FROM ctrace_select;').fetchone() == (1, 64)
    db.close()

    # 11. Corrupt / malformed .zst file
    corrupt_zst = os.path.join(tmpdir, "corrupt.zst")
    with open(corrupt_zst, "wb") as f:
        f.write(struct.pack("<I", 100) + b"\xde\xad\xbe\xef\x00\x01\x02")
    res = run(corrupt_zst)
    assert res.returncode != 0, f"[{label}] expected non-zero exit code for corrupt ZSTD file"


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <ctrace-decode-bin> <trace_schema.json> [ctrace_decode.py]")
        sys.exit(1)

    decoders = [([sys.argv[1]], "ctrace-decode")]
    if len(sys.argv) > 3:
        decoders.append(([sys.executable, sys.argv[3]], "ctrace_decode.py"))

    for decoder, label in decoders:
        with tempfile.TemporaryDirectory() as tmpdir:
            check_decoder(decoder, sys.argv[2], tmpdir, label)
        print(f"{label}: all edge case tests passed")


if __name__ == "__main__":
    main()
