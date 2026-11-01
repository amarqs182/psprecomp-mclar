#!/usr/bin/env python3
"""Find which recompiled functions call the GE/Display firmware thunks."""
import glob
import os
import re
import collections
import sys

BASE = "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)"

# Thunk address -> module::NID
TARGETS = {
    "003C4C58": "sceGe_user::0x1F6752AD",
    "003C4C60": "sceGe_user::0xA4FC06A4",
    "003C4C78": "sceGe_user::0xE47E40E4",
    "003C4C80": "sceDisplay::0x0E20F177",
    "003C4AE8": "sceGe_user::0xD61E6961",
    "003C4AD0": "sceGe_user::0x05572A5F",
    "003C4CE8": "sceGe_user::0xA569E425",
    "003C4D20": "sceDisplay::0xD979E9BF",
}

hits = collections.defaultdict(list)
files = sorted(glob.glob(os.path.join(BASE, "scratch/generated/mc3_funcs_*.c")))
for path in files:
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    cur = None
    for idx, line in enumerate(lines):
        m = re.search(r"psp_body_([0-9A-Fa-f]{8})", line)
        if m:
            cur = int(m.group(1), 16)
        for thunk in TARGETS:
            if "psp_import_" + thunk in line and cur is not None:
                hits[thunk].append((cur, os.path.basename(path), idx + 1))

for thunk, name in TARGETS.items():
    sites = hits.get(thunk, [])
    print("=== %s  %s : %d call site(s)" % (thunk, name, len(sites)))
    for fn, fname, ln in sites[:20]:
        print("    caller 0x%08X   %s:%d" % (fn, fname, ln))
    print()

# Cross-reference: which functions are reachable from the loader thread 0x000000D8?
print("=== all distinct caller functions touching GE/Display ===")
allfn = sorted({fn for sites in hits.values() for (fn, _, _) in sites})
for fn in allfn:
    print("  0x%08X" % fn)
