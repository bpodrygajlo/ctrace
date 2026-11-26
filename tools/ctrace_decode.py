#!/usr/bin/env python3
"""
ctrace_decode.py - Offline decoder for ctrace binary streams (bin and bin-zstd).
"""

import sys
import os
import json
import struct
import subprocess

TYPE_FORMATS = {
    "int8": ("b", 1),
    "uint8": ("B", 1),
    "int16": ("<h", 2),
    "uint16": ("<H", 2),
    "int32": ("<i", 4),
    "uint32": ("<I", 4),
    "int64": ("<q", 8),
    "uint64": ("<Q", 8),
    "float": ("<f", 4),
    "double": ("<d", 8),
    "bool": ("B", 1),
}

def flatten_schema_fields(fields, prefix=""):
    flat = []
    for f in fields:
        name = prefix + f["name"]
        if f.get("type") == "struct":
            sub_fields = f.get("fields", [])
            flat.extend(flatten_schema_fields(sub_fields, prefix=name + "_"))
        else:
            flat.append({
                "name": name,
                "type": f.get("type", "uint64"),
                "size": f.get("size", 8),
                "constants": f.get("constants", []),
            })
    return flat

def decode_stream(stream_data, schema):
    events_by_id = {}
    for ev in schema.get("events", []):
        ev_copy = dict(ev)
        ev_copy["flat_fields"] = flatten_schema_fields(ev.get("fields", []))
        events_by_id[ev["id"]] = ev_copy

    offset = 0
    total_len = len(stream_data)
    decoded = []

    while offset + 10 <= total_len:
        ev_id, ts_ns = struct.unpack_from("<HQ", stream_data, offset)
        offset += 10

        if ev_id not in events_by_id:
            # Unknown event or end of stream
            break

        ev_info = events_by_id[ev_id]
        record = {
            "event": ev_info["name"],
            "event_id": ev_id,
            "timestamp_ns": ts_ns,
            "fields": {}
        }

        for fld in ev_info["flat_fields"]:
            fname = fld["name"]
            ftype = fld["type"]

            if ftype == "string":
                if offset + 4 > total_len:
                    break
                slen, = struct.unpack_from("<I", stream_data, offset)
                offset += 4
                s_bytes = stream_data[offset:offset + slen]
                offset += slen
                record["fields"][fname] = s_bytes.decode("utf-8", errors="replace")
            elif ftype == "bytes":
                if offset + 4 > total_len:
                    break
                blen, = struct.unpack_from("<I", stream_data, offset)
                offset += 4
                b_data = stream_data[offset:offset + blen]
                offset += blen
                record["fields"][fname] = b_data.hex()
            elif ftype in TYPE_FORMATS:
                fmt, size = TYPE_FORMATS[ftype]
                if offset + size > total_len:
                    break
                val, = struct.unpack_from(fmt, stream_data, offset)
                offset += size
                if ftype == "bool":
                    val = bool(val)
                record["fields"][fname] = val
            elif ftype == "enum":
                # Enums stored as int32 or uint32
                if offset + 4 > total_len:
                    break
                val, = struct.unpack_from("<i", stream_data, offset)
                offset += 4
                # Lookup constant name if available
                c_name = str(val)
                for c in fld.get("constants", []):
                    if c.get("value") == val:
                        c_name = c.get("name", str(val))
                        break
                record["fields"][fname] = c_name
            else:
                sz = fld.get("size", 8)
                if sz == 4:
                    val, = struct.unpack_from("<I", stream_data, offset)
                else:
                    val, = struct.unpack_from("<Q", stream_data, offset)
                offset += sz
                record["fields"][fname] = val

        decoded.append(record)

    return decoded

def decompress_zstd_chunks(data):
    # Framed format: [ uint32_t comp_len ][ zstd_frame ]
    decompressed = bytearray()
    offset = 0
    total = len(data)

    while offset + 4 < total:
        comp_len, = struct.unpack_from("<I", data, offset)
        offset += 4
        if comp_len == 0 or offset + comp_len > total:
            break
        chunk = data[offset:offset + comp_len]
        offset += comp_len

        # Decompress using zstandard python module or cli
        try:
            import zstandard as zstd
            dctx = zstd.ZstdDecompressor()
            decompressed.extend(dctx.decompress(chunk))
        except ImportError:
            proc = subprocess.run(["zstd", "-d", "-c"], input=chunk, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
            decompressed.extend(proc.stdout)

    return bytes(decompressed)

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <trace_schema.json> <trace.bin|trace.bin.zst>")
        sys.exit(1)

    schema_path = sys.argv[1]
    data_path = sys.argv[2]

    with open(schema_path, "r") as f:
        schema = json.load(f)

    with open(data_path, "rb") as f:
        raw_data = f.read()

    if data_path.endswith(".zst") or schema.get("backend") == "bin-zstd":
        stream_data = decompress_zstd_chunks(raw_data)
    else:
        stream_data = raw_data

    events = decode_stream(stream_data, schema)
    for ev in events:
        field_strs = [f"{k}={v}" for k, v in ev["fields"].items()]
        print(f"[{ev['timestamp_ns']}] {ev['event']} {' '.join(field_strs)}")

if __name__ == "__main__":
    main()
