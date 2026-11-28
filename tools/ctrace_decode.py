#!/usr/bin/env python3
"""
ctrace_decode.py - High-performance offline decoder for ctrace binary streams.
"""

import sys
import os
import json
import struct
import subprocess
import argparse
import sqlite3
from datetime import datetime

CLOCK_ANCHOR_ID = 0
CLOCK_ANCHOR_SIZE = 18

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

ENUM_FORMATS = {
    (1, True): ("b", 1),
    (1, False): ("B", 1),
    (2, True): ("<h", 2),
    (2, False): ("<H", 2),
    (4, True): ("<i", 4),
    (4, False): ("<I", 4),
    (8, True): ("<q", 8),
    (8, False): ("<Q", 8),
}

def _flatten_fields_raw(fields, prefix=""):
    flat = []
    for f in fields:
        name = f["name"] if not prefix else f"{prefix}_{f['name']}"
        if f.get("type") == "struct":
            sub_fields = f.get("fields", [])
            flat.extend(_flatten_fields_raw(sub_fields, prefix=name))
        else:
            tag_field = f.get("tag_field", "")
            flat.append({
                "name": name,
                "type": f.get("type", "uint64"),
                "size": f.get("size", 8),
                "is_signed": f.get("is_signed", False),
                "constants": f.get("constants", []),
                "fields": f.get("fields", []),
                "counted_by": f.get("counted_by", ""),
                "tag_field": tag_field,
                # The tag is a sibling of the union, so it carries the same flattening prefix
                "tag_flat_name": (f"{prefix}_{tag_field}" if prefix else tag_field) if tag_field else "",
                "union_cases": f.get("union_cases", []),
            })
    return flat


def case_tag_value(case):
    """Discriminator value of a union case; older schemas stored it as array_index."""
    if "tag_val" in case:
        return case["tag_val"]
    return case.get("array_index")


def lookup_tag(record, fld):
    """Finds the discriminator value for a union field in the already decoded siblings."""
    if record is None:
        return None
    tag_flat = fld.get("tag_flat_name", "")
    if tag_flat and tag_flat in record:
        return record[tag_flat]
    tag_field = fld.get("tag_field") or fld.get("counted_by", "")
    if not tag_field:
        return None
    if tag_field in record:
        return record[tag_field]
    # Fallback: the most recently decoded field whose name ends with the tag name
    for k in reversed(list(record.keys())):
        if k.endswith(f"_{tag_field}"):
            return record[k]
    return None


def decode_fixed_array(arr_bytes, elem_schema):
    """Decodes the packed elements of a fixed-size array."""
    elem_type = elem_schema.get("type", "uint8")
    if elem_type in TYPE_FORMATS and elem_type != "bool":
        fmt, esz = TYPE_FORMATS[elem_type]
        cnt = len(arr_bytes) // esz
        code = fmt[1:] if fmt.startswith("<") else fmt
        return list(struct.unpack_from(f"<{cnt}{code}", arr_bytes, 0))
    esz = elem_schema.get("size", 1) or 1
    vals = []
    off = 0
    while off + esz <= len(arr_bytes):
        val, next_off = decode_field_value(arr_bytes, off, len(arr_bytes), elem_schema)
        if next_off is None or next_off <= off:
            break
        vals.append(val)
        off = next_off
    return vals

def flatten_schema_fields(fields):
    raw = _flatten_fields_raw(fields, "")
    used = set()
    result = []
    for ff in raw:
        base = ff["name"]
        unique = base
        counter = 2
        while unique in used:
            unique = f"{base}_{counter}"
            counter += 1
        used.add(unique)
        ff_copy = dict(ff)
        ff_copy["name"] = unique
        result.append(ff_copy)
    return result

def decode_field_value(stream_data, offset, total_len, fld, current_record=None):
    """Decodes a single flat field at offset, returning (value, new_offset) or (None, None) on truncation."""
    ftype = fld["type"]

    if ftype == "string":
        if offset + 4 > total_len:
            return None, None
        slen, = struct.unpack_from("<I", stream_data, offset)
        offset += 4
        if offset + slen > total_len:
            return None, None
        s_bytes = stream_data[offset:offset + slen]
        offset += slen
        return s_bytes.decode("utf-8", errors="replace"), offset

    if ftype == "bytes":
        if offset + 4 > total_len:
            return None, None
        blen, = struct.unpack_from("<I", stream_data, offset)
        offset += 4
        if offset + blen > total_len:
            return None, None
        b_data = stream_data[offset:offset + blen]
        offset += blen
        return b_data.hex(), offset

    if ftype == "fixed_array":
        sz = fld.get("size", 0)
        if offset + sz > total_len:
            return None, None
        arr_bytes = stream_data[offset:offset + sz]
        offset += sz
        sub_fields = fld.get("fields", [])
        if sub_fields:
            return decode_fixed_array(arr_bytes, sub_fields[0]), offset
        return list(arr_bytes), offset

    if ftype == "struct":
        sub_fields = fld.get("fields", [])
        sub_flat = flatten_schema_fields(sub_fields)
        res_dict = {}
        merged_rec = dict(current_record) if current_record else {}
        sub_off = offset
        for sf in sub_flat:
            val, next_off = decode_field_value(stream_data, sub_off, total_len, sf, merged_rec)
            if next_off is None:
                return None, None
            res_dict[sf["name"]] = val
            merged_rec[sf["name"]] = val
            sub_off = next_off
        return res_dict, sub_off

    if ftype == "union":
        if offset + 4 > total_len:
            return None, None
        u_len, = struct.unpack_from("<I", stream_data, offset)
        offset += 4
        if offset + u_len > total_len:
            return None, None
        u_data = stream_data[offset:offset + u_len]
        offset += u_len
        tag_val = lookup_tag(current_record, fld)
        if tag_val is not None:
            for c in fld.get("union_cases", []):
                if case_tag_value(c) == tag_val:
                    var_rec = dict(current_record)
                    val, _ = decode_field_value(u_data, 0, u_len, c, var_rec)
                    return {c["name"]: val}, offset
        return u_data.hex(), offset

    if ftype == "counted_array":
        if offset + 4 > total_len:
            return None, None
        blen, = struct.unpack_from("<I", stream_data, offset)
        offset += 4
        if offset + blen > total_len:
            return None, None
        end_offset = offset + blen
        sub_fields = fld.get("fields", [])
        if not sub_fields:
            b_data = stream_data[offset:end_offset]
            return b_data.hex(), end_offset

        elem_schema = sub_fields[0]
        elem_type = elem_schema.get("type", "uint8")
        if elem_schema.get("size") == 1 and elem_type in ("uint8", "int8", "char"):
            b_data = stream_data[offset:end_offset]
            return b_data.hex(), end_offset

        items = []
        sub_off = offset

        if elem_type == "struct":
            elem_flat = flatten_schema_fields(elem_schema.get("fields", []))
            while sub_off < end_offset:
                item_obj = {}
                item_start = sub_off
                for sf in elem_flat:
                    val, next_off = decode_field_value(stream_data, sub_off, end_offset, sf, item_obj)
                    if next_off is None:
                        return None, None
                    item_obj[sf["name"]] = val
                    sub_off = next_off
                if sub_off == item_start:
                    return None, None  # zero-sized element: corrupt length, never loop forever
                items.append(item_obj)
        else:
            while sub_off < end_offset:
                val, next_off = decode_field_value(stream_data, sub_off, end_offset, elem_schema)
                if next_off is None or next_off <= sub_off:
                    return None, None
                items.append(val)
                sub_off = next_off

        return items, end_offset

    if ftype in TYPE_FORMATS:
        fmt, size = TYPE_FORMATS[ftype]
        if offset + size > total_len:
            return None, None
        val, = struct.unpack_from(fmt, stream_data, offset)
        offset += size
        if ftype == "bool":
            val = bool(val)
        return val, offset

    if ftype == "enum":
        sz = fld.get("size", 4)
        is_signed = fld.get("is_signed", True)
        fmt, size = ENUM_FORMATS.get((sz, is_signed), ("<i", 4))
        if offset + size > total_len:
            return None, None
        val, = struct.unpack_from(fmt, stream_data, offset)
        offset += size
        c_name = str(val)
        for c in fld.get("constants", []):
            if c.get("value") == val:
                c_name = c.get("name", str(val))
                break
        return c_name, offset

    sz = fld.get("size", 8)
    fmt = "<I" if sz == 4 else ("<H" if sz == 2 else ("B" if sz == 1 else "<Q"))
    if offset + sz > total_len:
        return None, None
    val, = struct.unpack_from(fmt, stream_data, offset)
    offset += sz
    return val, offset

class FastEventDecoder:
    def __init__(self, ev_schema):
        self.name = ev_schema["name"]
        self.id = ev_schema["id"]
        self.flat_fields = flatten_schema_fields(ev_schema.get("fields", []))
        
        # Check if all fields are contiguous scalars for single-pass struct unpacking
        self.is_all_fixed = True
        fmt_parts = ["<HQ"]  # event_id (uint16), ts_ns (uint64)
        total_size = 10
        self.scalar_meta = []  # (name, is_bool, enum_map)

        for fld in self.flat_fields:
            ftype = fld["type"]
            if ftype in TYPE_FORMATS and ftype != "bool":
                code, sz = TYPE_FORMATS[ftype]
                fmt_parts.append(code[1:] if code.startswith("<") else code)
                total_size += sz
                self.scalar_meta.append((fld["name"], False, None))
            elif ftype == "bool":
                fmt_parts.append("B")
                total_size += 1
                self.scalar_meta.append((fld["name"], True, None))
            elif ftype == "enum":
                sz = fld.get("size", 4)
                is_signed = fld.get("is_signed", True)
                fmt, sz = ENUM_FORMATS.get((sz, is_signed), ("<i", 4))
                fmt_parts.append(fmt[1:] if fmt.startswith("<") else fmt)
                total_size += sz
                cmap = {c["value"]: c["name"] for c in fld.get("constants", [])}
                self.scalar_meta.append((fld["name"], False, cmap))
            else:
                self.is_all_fixed = False
                break

        if self.is_all_fixed:
            self.struct_obj = struct.Struct("".join(fmt_parts))
            self.fixed_size = total_size
        else:
            self.struct_obj = None

def decode_stream(stream_data, schema):
    """Returns (events sorted by timestamp, clock anchor or None).

    The clock anchor is the (trace_ns, wall_ns) pair from the first id-0 record."""
    decoders = {}
    for ev in schema.get("events", []):
        decoders[ev["id"]] = FastEventDecoder(ev)

    offset = 0
    total_len = len(stream_data)
    decoded = []
    anchor = None

    while offset + 10 <= total_len:
        ev_id, = struct.unpack_from("<H", stream_data, offset)
        if ev_id == CLOCK_ANCHOR_ID:
            if offset + CLOCK_ANCHOR_SIZE > total_len:
                raise ValueError(f"Truncated clock anchor at stream byte offset {offset}")
            trace_ns, wall_ns = struct.unpack_from("<QQ", stream_data, offset + 2)
            if anchor is None:
                anchor = (trace_ns, wall_ns)
            offset += CLOCK_ANCHOR_SIZE
            continue
        if ev_id not in decoders:
            raise ValueError(f"Unknown event_id {ev_id} at stream byte offset {offset}")

        dec = decoders[ev_id]
        if dec.struct_obj:
            if offset + dec.fixed_size > total_len:
                break
            vals = dec.struct_obj.unpack_from(stream_data, offset)
            offset += dec.fixed_size
            ts_ns = vals[1]
            fields = {}
            for idx, (name, is_bool, emap) in enumerate(dec.scalar_meta):
                v = vals[2 + idx]
                if is_bool:
                    v = bool(v)
                elif emap is not None:
                    v = emap.get(v, str(v))
                fields[name] = v
            decoded.append({
                "event": dec.name,
                "event_id": ev_id,
                "timestamp_ns": ts_ns,
                "fields": fields
            })
        else:
            ev_id, ts_ns = struct.unpack_from("<HQ", stream_data, offset)
            offset += 10
            record = {
                "event": dec.name,
                "event_id": ev_id,
                "timestamp_ns": ts_ns,
                "fields": {}
            }
            truncated = False
            for fld in dec.flat_fields:
                fname = fld["name"]
                val, next_offset = decode_field_value(stream_data, offset, total_len, fld, record["fields"])
                if next_offset is None:
                    truncated = True
                    break
                record["fields"][fname] = val
                offset = next_offset

            if truncated:
                raise ValueError(f"Truncated event '{dec.name}' in binary stream")
            decoded.append(record)

    if offset != total_len:
        raise ValueError(f"Trailing unparsed bytes at stream byte offset {offset} (stream size {total_len})")

    decoded.sort(key=lambda r: r["timestamp_ns"])
    return decoded, anchor


def format_timestamp(ts_ns, anchor, raw):
    """Wall-clock date when an anchor exists, else seconds since the trace clock origin."""
    if raw:
        return str(ts_ns)
    if anchor is not None:
        wall_ns = anchor[1] + (ts_ns - anchor[0])
        secs, nanos = divmod(wall_ns, 1_000_000_000)
        return f"{datetime.fromtimestamp(secs):%Y-%m-%d %H:%M:%S}.{nanos:09d}"
    secs, nanos = divmod(ts_ns, 1_000_000_000)
    return f"{secs}.{nanos:09d}"


def sqlite_column_type(ftype):
    if ftype in ("float", "double"):
        return "REAL"
    if ftype in ("string", "enum"):
        return "TEXT"
    if ftype == "bytes":
        return "BLOB"
    if ftype in TYPE_FORMATS:
        return "INTEGER"
    return "TEXT"  # arrays and unions keep their decoded text form


def sqlite_value(ftype, value):
    if value is None:
        return None
    if ftype == "bool":
        return int(value)
    if ftype == "bytes":
        return bytes.fromhex(value)
    if ftype == "uint64" and value > 0x7FFFFFFFFFFFFFFF:
        return value - (1 << 64)  # SQLite has no unsigned 64-bit; wrap like the sqlite backend does
    if isinstance(value, (list, dict)):
        return str(value)
    return value


def sql_ident(name):
    """Quotes an identifier so that field names such as 'order' or 'group' are usable."""
    return '"' + name.replace('"', '""') + '"'


def write_sqlite(db_path, schema, events, anchor, raw_ts):
    """One table per event type, ctrace_<event>(ts_ns, ts, <flattened fields...>)."""
    if os.path.exists(db_path):
        os.remove(db_path)
    db = sqlite3.connect(db_path)
    flat_by_name = {}
    for ev in schema.get("events", []):
        flat = flatten_schema_fields(ev.get("fields", []))
        flat_by_name[ev["name"]] = flat
        cols = ", ".join(f"{sql_ident(f['name'])} {sqlite_column_type(f['type'])}" for f in flat)
        db.execute(f"CREATE TABLE {sql_ident('ctrace_' + ev['name'])} (ts_ns INTEGER, ts TEXT{', ' + cols if cols else ''});")
    rows_by_name = {}
    for ev in events:
        flat = flat_by_name[ev["event"]]
        row = [ev["timestamp_ns"], format_timestamp(ev["timestamp_ns"], anchor, raw_ts)]
        row.extend(sqlite_value(f["type"], ev["fields"].get(f["name"])) for f in flat)
        rows_by_name.setdefault(ev["event"], []).append(row)
    for name, rows in rows_by_name.items():
        placeholders = ", ".join("?" * len(rows[0]))
        db.executemany(f"INSERT INTO {sql_ident('ctrace_' + name)} VALUES ({placeholders});", rows)
    db.commit()
    db.close()

def decompress_zstd_chunks(data):
    decompressed = bytearray()
    offset = 0
    total = len(data)

    dctx = None
    try:
        import zstandard as zstd
        dctx = zstd.ZstdDecompressor()
    except ImportError:
        pass

    concat_frames = bytearray()
    while offset + 4 <= total:
        comp_len, = struct.unpack_from("<I", data, offset)
        offset += 4
        if comp_len == 0 or offset + comp_len > total:
            raise ValueError(f"Corrupt zstd frame length {comp_len} at byte offset {offset - 4}")
        chunk = data[offset:offset + comp_len]
        offset += comp_len

        if dctx is not None:
            decompressed.extend(dctx.decompress(chunk))
        else:
            concat_frames.extend(chunk)

    if dctx is None and len(concat_frames) > 0:
        proc = subprocess.run(
            ["zstd", "-d", "-c"],
            input=bytes(concat_frames),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
        decompressed.extend(proc.stdout)

    return bytes(decompressed)

def main():
    parser = argparse.ArgumentParser(description="Decode a ctrace binary stream.")
    parser.add_argument("schema", help="trace_schema.json written by ctrace-gen")
    parser.add_argument("trace", help="trace.bin or trace.bin.zst")
    parser.add_argument("-o", "--output", metavar="FILE", help="write the text output to FILE instead of stdout")
    parser.add_argument("--sqlite", metavar="DB", help="also write the decoded events to a SQLite database")
    parser.add_argument("--ns", action="store_true", help="print raw nanosecond timestamps instead of dates")
    parser.add_argument("-q", "--quiet", action="store_true", help="no text output")
    args = parser.parse_args()

    with open(args.schema, "r") as f:
        schema = json.load(f)

    with open(args.trace, "rb") as f:
        raw_data = f.read()

    if args.trace.endswith(".zst") or schema.get("backend") == "bin-zstd":
        stream_data = decompress_zstd_chunks(raw_data)
    else:
        stream_data = raw_data

    events, anchor = decode_stream(stream_data, schema)

    if args.sqlite:
        write_sqlite(args.sqlite, schema, events, anchor, args.ns)

    if args.quiet:
        return

    out = open(args.output, "w") if args.output else sys.stdout
    # Fast buffered output (chunking into 64 KB writes)
    chunk = []
    chunk_len = 0
    for ev in events:
        field_strs = [f"{k}={v}" for k, v in ev["fields"].items()]
        ts = format_timestamp(ev["timestamp_ns"], anchor, args.ns)
        line = f"[{ts}] {ev['event']} {' '.join(field_strs)}\n"
        chunk.append(line)
        chunk_len += len(line)
        if chunk_len >= 65536:
            out.write("".join(chunk))
            chunk.clear()
            chunk_len = 0
    if chunk:
        out.write("".join(chunk))
    if out is not sys.stdout:
        out.close()

if __name__ == "__main__":
    main()
