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
    assert "union_cases" in u_fld and len(u_fld["union_cases"]) == 2, f"Expected 2 union cases, got {u_fld.get('union_cases')}"

    # 2. Decode binary stream
    cmd = [sys.executable, decode_tool, schema_path, bin_path]
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

    print("All FAPI union, fixed-array, and dynamic count binding assertions PASSED!")

if __name__ == "__main__":
    main()
