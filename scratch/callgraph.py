#!/usr/bin/env python3
"""Build the recompiled call graph and report reachability from a root.

Usage: callgraph.py <root_hex> [target_hex ...]

Finds every recompiled function reachable from <root> by direct call,
then reports which of the given targets are reachable and, for each
reachable target, the shortest call path.
"""
import collections
import glob
import os
import re
import sys

BASE = "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)"

root = int(sys.argv[1], 16)
targets = {int(a, 16) for a in sys.argv[2:]}

# graph: caller -> set(callee)
graph = collections.defaultdict(set)

files = sorted(glob.glob(os.path.join(BASE, "scratch/generated/mc3_funcs_*.c")))
for path in files:
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    # Split on function boundaries so we know the owner of each call
    parts = re.split(r"static void psp_body_([0-9A-Fa-f]{8})\(", text)
    for i in range(1, len(parts), 2):
        owner = int(parts[i], 16)
        body = parts[i + 1]
        for sym in re.findall(r"psp_(?:func|at)_([0-9A-Fa-f]{8})", body):
            graph[owner].add(int(sym, 16))

print("functions parsed: %d" % len(graph))

# BFS from root
parent = {root: None}
q = collections.deque([root])
order = []
while q:
    n = q.popleft()
    order.append(n)
    for c in sorted(graph.get(n, ())):
        if c not in parent:
            parent[c] = n
            q.append(c)

print("reachable from 0x%08X: %d functions" % (root, len(parent)))
print()

for t in sorted(targets):
    if t not in parent:
        print("TARGET 0x%08X : NOT reachable" % t)
        continue
    path = []
    n = t
    while n is not None:
        path.append(n)
        n = parent[n]
    path.reverse()
    print("TARGET 0x%08X : REACHABLE" % t)
    print("    path: " + " -> ".join("0x%08X" % x for x in path))
    print()

# Which functions in the reachable set call GE/Display thunks?
GE_THUNKS = {
    0x003C4C58: "sceGe 0x1F6752AD",
    0x003C4C60: "sceGe 0xA4FC06A4",
    0x003C4C78: "sceGe 0xE47E40E4 (ListEnqueue)",
    0x003C4C80: "sceDisplay 0x0E20F177",
    0x003C4AE8: "sceGe 0xD61E6961",
    0x003C4AD0: "sceGe 0x05572A5F",
    0x003C4CE8: "sceGe 0xA569E425",
    0x003C4D20: "sceDisplay 0xD979E9BF (SetFrameBuf)",
}
print("=== GE/Display thunks called from the reachable set ===")
found = 0
for fn in sorted(parent):
    for thunk, name in GE_THUNKS.items():
        if thunk in graph.get(fn, ()):
            print("    0x%08X calls %s" % (fn, name))
            found += 1
if not found:
    print("    (none)")
