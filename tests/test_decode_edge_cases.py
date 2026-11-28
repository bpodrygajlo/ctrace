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
        res = subprocess.run([decoder_bin, schema_path, unordered_bin], capture_output=True, text=True, timeout=5)
        assert res.returncode == 0, res.stderr
        assert [l.split("]")[0] for l in res.stdout.splitlines()] == ["[1000", "[2000", "[3000"], res.stdout

        # 7. Test corrupt / malformed .zst file
        corrupt_zst = os.path.join(tmpdir, "corrupt.zst")
        with open(corrupt_zst, "wb") as f:
            f.write(struct.pack("<I", 100) + b"\xde\xad\xbe\xef\x00\x01\x02")
        res = subprocess.run([decoder_bin, schema_path, corrupt_zst], capture_output=True, timeout=5)
        assert res.returncode != 0, f"Expected non-zero exit code for corrupt ZSTD file, got {res.returncode}"

    print("All ctrace-decode edge case tests passed successfully!")

if __name__ == "__main__":
    main()
