#!/usr/bin/env python3
"""Checks the content of a CTF trace through babeltrace2."""
import subprocess
import sys


def main():
    babeltrace = sys.argv[1]
    trace_dir = sys.argv[2]
    out = subprocess.run([babeltrace, trace_dir], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, check=True).stdout
    print(out)
    lines = out.splitlines()

    # Fields following a counted struct array with bytes payloads land at the right offset:
    # 2 items x (kind 1 + f1 1 + f2 1 + enum 4 + bytes 4+2) = 26 bytes
    list_lines = [l for l in lines if "ctf_list_event" in l]
    assert len(list_lines) == 1, list_lines
    assert "list_items_len = 26" in list_lines[0], list_lines[0]
    assert "marker = 4242" in list_lines[0], list_lines[0]

    # TSDL keywords used as field names are escaped and still readable
    reserved = [l for l in lines if "ctf_reserved" in l]
    assert len(reserved) == 1, reserved
    assert "stream = 1" in reserved[0] and "event = 2" in reserved[0], reserved[0]

    assert sum(1 for l in lines if "net_rx" in l) == 30
    print("CTF content assertions PASSED!")


if __name__ == "__main__":
    main()
