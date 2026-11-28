#!/usr/bin/env python3
"""
Test edge cases for ctrace-decode:
- Truncated records (should return non-zero exit code, not crash or loop infinitely)
- Unknown event IDs (should return non-zero exit code)
- Empty trace files (should return exit code 0)
- Trailing garbage bytes (should return non-zero exit code)
"""

import sys
import os
import subprocess
import tempfile
import struct
import sqlite3
import re

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <ctrace-decode-bin> <trace_schema.json>")
        sys.exit(1)

    decoder_bin = sys.argv[1]
    schema_path = sys.argv[2]

    with tempfile.TemporaryDirectory() as tmpdir:
        # 1. Test empty trace file -> should succeed with exit code 0
        empty_bin = os.path.join(tmpdir, "empty.bin")
        open(empty_bin, "wb").close()
        res = subprocess.run([decoder_bin, schema_path, empty_bin], capture_output=True)
        assert res.returncode == 0, f"Expected 0 for empty trace, got {res.returncode}"

        # 2. Test unknown event ID -> should return non-zero exit code
        unknown_bin = os.path.join(tmpdir, "unknown.bin")
        with open(unknown_bin, "wb") as f:
            f.write(struct.pack("<HQ", 9999, 12345678))
        res = subprocess.run([decoder_bin, schema_path, unknown_bin], capture_output=True)
        assert res.returncode != 0, f"Expected non-zero exit code for unknown event ID, got {res.returncode}"

        # 3. Test truncated record (header says event 2, but only 4 payload bytes instead of full payload)
        trunc_bin = os.path.join(tmpdir, "truncated.bin")
        with open(trunc_bin, "wb") as f:
            f.write(struct.pack("<HQ", 2, 12345678) + b"\x01\x02\x03\x04")
        res = subprocess.run([decoder_bin, schema_path, trunc_bin], capture_output=True, timeout=5)
        assert res.returncode != 0, f"Expected non-zero exit code for truncated record, got {res.returncode}"

        # 4. Test trailing garbage bytes
        garbage_bin = os.path.join(tmpdir, "garbage.bin")
        payload = b"testpayload"
        valid_rec = struct.pack("<HQ", 2, 1000000) + struct.pack("<i", 42) + struct.pack("<Q", 64) + struct.pack("<I", len(payload)) + payload
        with open(garbage_bin, "wb") as f:
            f.write(valid_rec + b"\xaa\xbb\xcc")
        res = subprocess.run([decoder_bin, schema_path, garbage_bin], capture_output=True, timeout=5)
        assert res.returncode != 0, f"Expected non-zero exit code for trailing unparsed bytes, got {res.returncode}"

        # 5. Test truncated CountedArray payload (event 1 has counted array `sensors`)
        # blen claims 64 bytes, but only 4 bytes follow -> must exit non-zero immediately without hanging
        trunc_ca_bin = os.path.join(tmpdir, "trunc_ca.bin")
        with open(trunc_ca_bin, "wb") as f:
            # event 1: id=1, ts=1000, batch_id=1, num_sensors=2, sensors blen=64, but only 4 bytes follow
            f.write(struct.pack("<HQ", 1, 1000) + struct.pack("<iH", 101, 2) + struct.pack("<I", 64) + b"\x01\x02\x03\x04")
        res = subprocess.run([decoder_bin, schema_path, trunc_ca_bin], capture_output=True, timeout=5)
        assert res.returncode != 0, f"Expected non-zero exit code for truncated CountedArray, got {res.returncode}"

        # 6. Events are written out ordered by timestamp, whatever their order in the file
        unordered_bin = os.path.join(tmpdir, "unordered.bin")
        rec = lambda ts, fd: struct.pack("<HQ", 2, ts) + struct.pack("<i", fd) + struct.pack("<Q", 64) + struct.pack("<I", 0)
        with open(unordered_bin, "wb") as f:
            f.write(rec(3000, 3) + rec(1000, 1) + rec(2000, 2))
        res = subprocess.run([decoder_bin, schema_path, unordered_bin, "--ns"], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0, res.stderr
        assert [l.split("]")[0] for l in res.stdout.splitlines()] == ["[1000", "[2000", "[3000"], res.stdout

        # Without a clock anchor timestamps are printed as seconds; with one, as local dates
        res = subprocess.run([decoder_bin, schema_path, unordered_bin], capture_output=True, text=True, timeout=5)
        assert res.stdout.startswith("[0.000001000] "), res.stdout
        anchor = struct.pack("<HQQ", 0, 5000, 1_700_000_000_123_456_789)  # trace_ns=5000 <-> 2023-11-14T22:13:20.123456789Z
        anchored_bin = os.path.join(tmpdir, "anchored.bin")
        with open(anchored_bin, "wb") as f:
            f.write(anchor + rec(6000, 1))  # event 1000 ns after the anchor
        res = subprocess.run([decoder_bin, schema_path, anchored_bin], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0, res.stderr
        assert re.match(r"^\[20\d\d-\d\d-\d\d \d\d:\d\d:\d\d\.123457789\] net_rx ", res.stdout), res.stdout

        # An anchor alone is a valid, empty trace
        anchor_only = os.path.join(tmpdir, "anchor_only.bin")
        with open(anchor_only, "wb") as f:
            f.write(anchor)
        res = subprocess.run([decoder_bin, schema_path, anchor_only], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0 and res.stdout == "", (res.returncode, res.stdout)

        # SQLite output: one table per event, scalars as numbers, bytes as BLOB, -q silences text
        db_path = os.path.join(tmpdir, "out.db")
        valid_bin = os.path.join(tmpdir, "valid.bin")
        with open(valid_bin, "wb") as f:
            f.write(anchor + valid_rec)
        res = subprocess.run([decoder_bin, schema_path, valid_bin, "--sqlite", db_path, "-q"], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0 and res.stdout == "", (res.returncode, res.stdout, res.stderr)
        db = sqlite3.connect(db_path)
        row = db.execute("SELECT ts_ns, ts, arg_0, len, payload FROM ctrace_net_rx;").fetchone()
        db.close()
        assert row[0] == 1000000 and row[2] == 42 and row[3] == 64 and row[4] == payload, row
        assert re.match(r"^20\d\d-\d\d-\d\d \d\d:\d\d:\d\d\.\d{9}$", row[1]), row

        # 7. Test corrupt / malformed .zst file
        corrupt_zst = os.path.join(tmpdir, "corrupt.zst")
        with open(corrupt_zst, "wb") as f:
            f.write(struct.pack("<I", 100) + b"\xde\xad\xbe\xef\x00\x01\x02")
        res = subprocess.run([decoder_bin, schema_path, corrupt_zst], capture_output=True, timeout=5)
        assert res.returncode != 0, f"Expected non-zero exit code for corrupt ZSTD file, got {res.returncode}"

    print("All ctrace-decode edge case tests passed successfully!")

if __name__ == "__main__":
    main()
