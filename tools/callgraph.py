#!/usr/bin/env python3
"""Build a call graph from the generated recompiled C sources and trace paths.

Usage:
    python3 tools/callgraph.py --target 0x002A8C50 [--from 0x000000D8] [--depth 6]
    python3 tools/callgraph.py --stats
"""
import argparse
import collections
import os
import re

BODY_RE = re.compile(r"^static void psp_body_([0-9A-Fa-f]+)\(")
FUNC_RE = re.compile(r"psp_func_([0-9A-Fa-f]+)\s*\(\s*\)")
AT_RE = re.compile(r"psp_at_([0-9A-Fa-f]+)\s*\(\s*\)")
IMPORT_RE = re.compile(r"psp_import_([0-9A-Fa-f]+)\s*\(\s*\)")
ADDR_RE = re.compile(r"0x([0-9A-Fa-f]{8})u")


def parse_dir(gen_dir):
    """Return callers: {callee_addr: set(caller_addr)}, and direct-call edges."""
    callers = collections.defaultdict(set)
    edges = collections.defaultdict(set)   # caller -> callees
    func_addrs = set()

    for name in sorted(os.listdir(gen_dir)):
        if not (name.startswith("mc3_funcs_") and name.endswith(".c")):
            continue
        path = os.path.join(gen_dir, name)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()

        # Split into function bodies so we know the current caller.
        chunks = re.split(r"(?=^static void psp_body_)", text, flags=re.M)
        for chunk in chunks:
            bm = BODY_RE.match(chunk)
            if not bm:
                continue
            caller = int(bm.group(1), 16)
            func_addrs.add(caller)

            for callee_hex in FUNC_RE.findall(chunk):
                callee = int(callee_hex, 16)
                if callee == caller:
                    continue
                callers[callee].add(caller)
                edges[caller].add(callee)

            for callee_hex in AT_RE.findall(chunk):
                callee = int(callee_hex, 16)
                callers[callee].add(caller)
                edges[caller].add(callee)

    return callers, edges, func_addrs


def find_paths(edges, target, roots, max_depth):
    """Find call paths from any root to target using BFS on the reverse graph."""
    rev = collections.defaultdict(set)
    for caller, callees in edges.items():
        for callee in callees:
            rev[callee].add(caller)

    results = []
    for root in roots:
        # BFS from root
        queue = collections.deque([(root, [root])])
        seen = {root}
        while queue:
            node, path = queue.popleft()
            if len(path) > max_depth:
                continue
            if node == target and len(path) > 1:
                results.append(path)
                continue
            for nxt in sorted(edges.get(node, ())):
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append((nxt, path + [nxt]))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="scratch/generated")
    ap.add_argument("--target", type=lambda s: int(s, 16))
    ap.add_argument("--from", dest="roots", action="append", type=lambda s: int(s, 16))
    ap.add_argument("--depth", type=int, default=8)
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--show-callers", type=lambda s: int(s, 16))
    args = ap.parse_args()

    callers, edges, func_addrs = parse_dir(args.dir)

    if args.stats:
        print(f"functions parsed: {len(func_addrs)}")
        print(f"call edges: {sum(len(v) for v in edges.values())}")
        targets = [a for a in func_addrs if len(callers.get(a, ())) == 0]
        print(f"functions with no caller (roots/unreachable): {len(targets)}")
        return

    if args.show_callers is not None:
        t = args.show_callers
        print(f"callers of 0x{t:08X}: {len(callers.get(t, ()))}")
        for c in sorted(callers.get(t, ()))[:40]:
            print(f"  0x{c:08X}")
        return

    if args.target is None:
        ap.error("need --target or --stats")

    roots = args.roots or [0x000000D8, 0x000005A4]
    print(f"looking for paths to 0x{args.target:08X} from {[hex(r) for r in roots]}")
    paths = find_paths(edges, args.target, roots, args.depth)
    if not paths:
        print("  NO PATH FOUND")
        print(f"\ndirect callers of target: {len(callers.get(args.target, ()))}")
        for c in sorted(callers.get(args.target, ()))[:20]:
            print(f"  0x{c:08X}")
        print(f"\nroots reach {len(roots)} direct children:")
        for r in roots:
            print(f"  0x{r:08X} -> {len(edges.get(r, ()))} callees")
    else:
        for p in paths[:20]:
            print("  " + " -> ".join(f"0x{a:08X}" for a in p))


if __name__ == "__main__":
    main()
