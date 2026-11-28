#!/usr/bin/env python3
import sys
import json
import os
import subprocess

def main():
    schema_path = sys.argv[1]
    bin_path = sys.argv[2]
    decode_tool = sys.argv[3]

    with open(schema_path, "r") as f:
        schema = json.load(f)

    # 1. Verify schema
    tti_ev = next(e for e in schema["events"] if e["name"] == "fapi_tti")
    req_fld = tti_ev["fields"][0]
    sub_names = [f["name"] for f in req_fld["fields"]]

    # Ensure rb_bitmap is NOT flattened into 36 separate fields!
    assert "rb_bitmap" in sub_names, f"Expected 'rb_bitmap' in {sub_names}"
    assert "rb_bitmap_0" not in sub_names, "rb_bitmap should not be unrolled into rb_bitmap_0..35"
    
    rb_fld = next(f for f in req_fld["fields"] if f["name"] == "rb_bitmap")
    assert rb_fld["type"] == "fixed_array", f"Expected fixed_array, got {rb_fld['type']}"
    assert rb_fld["size"] == 36, f"Expected size 36, got {rb_fld['size']}"

    # Ensure cce_list is a counted_array bound to num_cce
    cce_fld = next(f for f in req_fld["fields"] if f["name"] == "cce_list")
    assert cce_fld["type"] == "counted_array", f"Expected counted_array, got {cce_fld['type']}"
    assert cce_fld["counted_by"] == "num_cce", f"Expected counted_by 'num_cce', got {cce_fld['counted_by']}"

    # Ensure union in fapi_pdu_event
    pdu_ev = next(e for e in schema["events"] if e["name"] == "fapi_pdu_event")
    pdu_struct = pdu_ev["fields"][0]
    u_fld = next(f for f in pdu_struct["fields"] if f["name"] == "u")
    assert u_fld["type"] == "union", f"Expected union type, got {u_fld['type']}"
    assert u_fld["tag_field"] == "pdu_type", f"Expected tag_field 'pdu_type', got {u_fld['tag_field']}"
    assert "union_cases" in u_fld and len(u_fld["union_cases"]) == 3, f"Expected 3 union cases, got {u_fld.get('union_cases')}"

    # Ensure unbound union in fapi_raw_event
    raw_ev = next(e for e in schema["events"] if e["name"] == "fapi_raw_event")
    wm_struct = raw_ev["fields"][0]
    m_fld = next(f for f in wm_struct["fields"] if f["name"] == "m")
    assert m_fld["type"] == "union", f"Expected union type for m, got {m_fld['type']}"
    assert "union_cases" not in m_fld or len(m_fld["union_cases"]) == 0

    # Ensure dynamic variant union in fapi_dyn_pdu_event
    dyn_ev = next(e for e in schema["events"] if e["name"] == "fapi_dyn_pdu_event")
    dyn_struct = dyn_ev["fields"][0]
    dyn_u = next(f for f in dyn_struct["fields"] if f["type"] == "union")
    assert len(dyn_u["union_cases"]) == 2

    # Negative and large unsigned tag values must survive into the schema
    res_ev = next(e for e in schema["events"] if e["name"] == "fapi_result_event")
    res_u = next(f for f in res_ev["fields"][0]["fields"] if f["type"] == "union")
    assert sorted(c["tag_val"] for c in res_u["union_cases"]) == [-1, 0], res_u["union_cases"]
    big_ev = next(e for e in schema["events"] if e["name"] == "fapi_big_tag_event")
    big_u = next(f for f in big_ev["fields"][0]["fields"] if f["type"] == "union")
    assert sorted(c["tag_val"] for c in big_u["union_cases"]) == [7, 0xFFFFFFFF], big_u["union_cases"]

    # Two unions in one struct keep their own tag field and cases
    two_ev = next(e for e in schema["events"] if e["name"] == "fapi_two_unions_event")
    two_fields = two_ev["fields"][0]["fields"]
    u1 = next(f for f in two_fields if f["name"] == "u1")
    u2 = next(f for f in two_fields if f["name"] == "u2")
    assert u1["tag_field"] == "t1" and sorted(c["name"] for c in u1["union_cases"]) == ["a", "b"], u1
    assert u2["tag_field"] == "t2" and sorted(c["name"] for c in u2["union_cases"]) == ["c", "d"], u2

    # A bare union pointer argument is a pointer-typed union field
    word_ev = next(e for e in schema["events"] if e["name"] == "fapi_word_event")
    assert word_ev["fields"][0]["type"] == "union" and word_ev["fields"][0].get("is_pointer") is True

    # A directly passed fixed array is a fixed_array of its element type
    samp_ev = next(e for e in schema["events"] if e["name"] == "fapi_samples_event")
    samp = samp_ev["fields"][0]
    assert samp["type"] == "fixed_array" and samp["size"] == 12 and samp["fields"][0]["type"] == "uint16", samp

    # Anonymous struct members are spliced into the parent
    anon_ev = next(e for e in schema["events"] if e["name"] == "fapi_anon_struct_event")
    assert [f["name"] for f in anon_ev["fields"][0]["fields"]] == ["id", "x", "y"], anon_ev

    # A fixed array of structs is a constant-count counted array (elements are packed, not memcpy'd)
    cells_ev = next(e for e in schema["events"] if e["name"] == "fapi_cells_event")
    cells_fld = next(f for f in cells_ev["fields"][0]["fields"] if f["name"] == "cells")
    assert cells_fld["type"] == "counted_array" and cells_fld["array_len"] == 8 and "counted_by" not in cells_fld, cells_fld
    assert cells_fld["fields"][0]["type"] == "struct"

    # Skipped field is absent from the schema
    skip_ev = next(e for e in schema["events"] if e["name"] == "fapi_skip_event")
    assert [f["name"] for f in skip_ev["fields"][0]["fields"]] == ["n", "z"], skip_ev

    # 2. Decode binary stream
    if decode_tool.endswith(".py"):
        cmd = [sys.executable, decode_tool, schema_path, bin_path]
    else:
        cmd = [decode_tool, schema_path, bin_path]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
    out = res.stdout
    print("Decoded output:\n" + out)

    # Check decoded content
    assert "fapi_tti" in out
    assert "'cce_index': 10" in out
    assert "'cce_index': 20" in out
    # Verify union cases decoded correctly
    assert "dlsch_pdu" in out
    assert "'tti': 1001" in out
    assert "pdcch_pdu" in out
    assert "'freq_domain': 43981" in out # 0xABCD = 43981

    # Verify multiple tag values mapping to same union case (tag 2 -> dlsch_pdu)
    assert "'tti': 2002" in out
    assert "'mcs': 14" in out

    # Verify fixed array inside element of counted struct array
    assert "fapi_dci_event" in out
    assert "'size': 5" in out
    assert "'bits': [170, 187, 204" in out

    # Verify anonymous discriminated union
    assert "fapi_anon_event" in out
    assert "'pucch': {'b': 8888, 'c': 99}" in out

    # Verify count-bound byte payload decoded as hex string (like TRACE_BYTES)
    assert "fapi_rx_event" in out
    assert "deadbeef" in out

    # Verify unbound union (pthread_mutex_t)
    assert "fapi_raw_event" in out
    assert "wm_n=42" in out

    # Verify union variant holding count-bound array
    assert "fapi_dyn_pdu_event" in out
    assert "'rnti': 4660" in out
    assert "'cce': [5, 9]" in out
    assert "'ta_val': 31" in out

    lines = out.splitlines()

    def event_lines(name):
        return [l for l in lines if f" {name} " in l]

    # Unbound union (pthread_mutex_t) is dumped as zero bytes of sizeof(pthread_mutex_t)
    raw_line = event_lines("fapi_raw_event")[0]
    m_val = raw_line.split("wm_m=")[1].split()[0]
    assert len(m_val) >= 2 * 24 and set(m_val) == {"0"}, raw_line

    # Unmatched tag falls back to the raw union bytes (tti=0x11223344, little-endian).
    # Field names come from the first call site, so every fapi_pdu_event is named pdu1_*.
    pdu4_line = [l for l in event_lines("fapi_pdu_event") if "pdu1_pdu_type=99" in l][0]
    assert "pdu1_u=4433221100000000" in pdu4_line, pdu4_line

    # Negative tag selects the error variant
    assert "'err': {'code': -22}" in event_lines("fapi_result_event")[0]

    # Large unsigned tag selects its variant
    assert "'a': {'a': 200}" in event_lines("fapi_big_tag_event")[0]

    # Each union follows its own tag
    two_line = event_lines("fapi_two_unions_event")[0]
    assert "two_u1={'b': {'b1': 70000}}" in two_line, two_line
    assert "two_u2={'c': {'c1': 9}}" in two_line, two_line

    # Nested struct inside a variant is flattened identically by both decoders
    nested_line = event_lines("fapi_nested_event")[0]
    assert "nested_u={'nv': {'hdr_x': 3, 'hdr_y': 4, 'body': 500}}" in nested_line, nested_line

    # Union by pointer dumps the pointee bytes; NULL dumps nothing
    word_lines = event_lines("fapi_word_event")
    assert any("wp=01020304" in l for l in word_lines), word_lines
    assert any(l.rstrip().endswith("wp=") for l in word_lines), word_lines

    # Two bools per counted struct element
    flags_line = event_lines("fapi_flags_event")[0]
    assert "fl_flags=[{'on': True, 'err': False, 'lvl': 1}, {'on': False, 'err': True, 'lvl': 2}]" in flags_line, flags_line

    # Directly passed fixed array
    assert "samples=[1, 2, 3, 4, 5, 6]" in event_lines("fapi_samples_event")[0]

    # Anonymous struct members
    anon_line = event_lines("fapi_anon_struct_event")[0]
    assert "as_id=12 as_x=7 as_y=9" in anon_line, anon_line

    # Padded struct elements: 8 elements, each read from its own offset, enum names quoted inside dicts
    cells_line = event_lines("fapi_cells_event")[0]
    assert "cells_cells=[{'a': 1, 'rc': 'FAPI_RC_OK', 'b': 10}, {'a': 7, 'rc': 'FAPI_RC_BAD', 'b': 16909060}, " in cells_line, cells_line
    assert cells_line.count("{'a': 0, 'rc': 'FAPI_RC_OK', 'b': 0}") == 5, cells_line
    assert cells_line.rstrip().endswith("{'a': 77, 'rc': 'FAPI_RC_BAD', 'b': 7777}]"), cells_line

    # Skipped field never reaches the trace
    skip_line = event_lines("fapi_skip_event")[0]
    assert skip_line.rstrip().endswith("fapi_skip_event sk_n=1 sk_z=2"), skip_line

    # Output is ordered by timestamp
    stamps = [int(l.split("]")[0][1:]) for l in lines if l.startswith("[")]
    assert stamps == sorted(stamps), "decoder output is not sorted by timestamp"

    print("All FAPI union, fixed-array, and dynamic count binding assertions PASSED!")

if __name__ == "__main__":
    main()
