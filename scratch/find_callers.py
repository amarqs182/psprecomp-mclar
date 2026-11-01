#!/usr/bin/env python3
"""Trace callers of a set of functions within the recompiled sources.

Usage: find_callers.py 0x002A8C50 0x002F048C ...
Prints, for each target, every recompiled function that calls it, plus
the call sites.
"""
import glob
import os
import re
import sys
import collections

BASE = "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)"

targets = {}
for arg in sys.argv[1:]:
    targets[arg.upper().replace("0X", "")] = "0x" + arg.upper().replace("0X", "")

# Build a map: callee symbol -> callers
# symbols are psp_func_XXXXXXXX (own function) or psp_at_XXXXXXXX (address entry)
callers = collections.defaultdict(list)

files = sorted(glob.glob(os.path.join(BASE, "scratch/generated/mc3_funcs_*.c")))
for path in files:
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    cur = None
    for idx, line in enumerate(lines):
        m = re.search(r"psp_body_([0-9A-Fa-f]{8})", line)
        if m:
            cur = int(m.group(1), 16)
        for sym in re.findall(r"psp_(?:func|at)_([0-9A-Fa-f]{8})", line):
            key = sym.upper()
            if key in targets and cur is not None:
                callers[key].append((cur, os.path.basename(path), idx + 1, line.strip()))

for key, disp in targets.items():
    sites = callers.get(key, [])
    print("=== callers of %s : %d" % (disp, len(sites)))
    for fn, fname, ln, txt in sites[:30]:
        print("    0x%08X  %s:%d" % (fn, fname, ln))
    print()
