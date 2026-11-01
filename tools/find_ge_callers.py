#!/usr/bin/env python3
"""Map which recompiled functions call the PSP GE/Display import stubs.

Reads the generated C sources under scratch/generated/ and reports, for every
GE/Display import stub, the containing function address and source line.

Usage:
    python3 tools/find_ge_callers.py [--dir scratch/generated]
"""
import argparse
import json
import os
import re
import sys

# Firmware libraries that own the rendering entry points.
RENDER_LIBS = ("sceGe_user", "sceDisplay", "sceGuDraw", "sceGu", "sceDma", "sceUtility")

STUB_RE = re.compile(
    r"void\s+psp_import_([0-9A-Fa-f]+)\s*\(void\)\s*\{[^}]*?psp_hle_call\((0x[0-9A-Fa-f]+)u\)"
)
NID_RE = re.compile(r"/\*\s*(\S+)\s*::\s*NID\s+(0x[0-9A-Fa-f]+)\s*\*/")
BODY_RE = re.compile(r"^static void psp_body_([0-9A-Fa-f]+)\(")
CALL_RE = re.compile(r"psp_import_([0-9A-Fa-f]+)\s*\(\s*\)\s*;")


def load_imports(path):
    """Return (stub_addr -> nid, nid -> library) from the generated imports file."""
    stub_nid = {}
    nid_lib = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    for lib, nid_hex in NID_RE.findall(text):
        nid_lib[int(nid_hex, 16)] = lib
    for stub_hex, nid_hex in STUB_RE.findall(text):
        stub_nid[int(stub_hex, 16)] = int(nid_hex, 16)
    return stub_nid, nid_lib


def scan_callers(gen_dir, stub_nid, nid_lib):
    """Find call sites of the interesting stubs in every generated .c file."""
    wanted = {
        addr: nid
        for addr, nid in stub_nid.items()
        if any(
            nid_lib.get(nid, "").lower().startswith(r.lower())
            for r in RENDER_LIBS
            if r
        )
    }
    hits = {}

    for name in sorted(os.listdir(gen_dir)):
        if not (name.startswith("mc3_funcs_") and name.endswith(".c")):
            continue
        path = os.path.join(gen_dir, name)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()

        current = None
        for lineno, line in enumerate(lines, 1):
            bm = BODY_RE.match(line)
            if bm:
                current = int(bm.group(1), 16)
            for stub_hex in CALL_RE.findall(line):
                addr = int(stub_hex, 16)
                if addr in wanted:
                    hits.setdefault((current, addr), []).append((name, lineno))

    return wanted, hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="scratch/generated")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    imports_path = os.path.join(args.dir, "mc3_imports.c")
    if not os.path.isfile(imports_path):
        sys.exit(f"missing {imports_path}")

    stub_nid, nid_lib = load_imports(imports_path)
    wanted, hits = scan_callers(args.dir, stub_nid, nid_lib)

    if args.json:
        out = []
        for (fn_addr, stub), sites in sorted(hits.items()):
            nid = stub_nid[stub]
            out.append({
                "function": f"0x{fn_addr:08X}" if fn_addr is not None else None,
                "stub": f"0x{stub:08X}",
                "nid": f"0x{nid:08X}",
                "library": nid_lib.get(nid, "?"),
                "sites": [f"{f}:{l}" for f, l in sites],
            })
        print(json.dumps(out, indent=2))
        return

    print(f"GE/Display stubs tracked: {len(wanted)}")
    print(f"Functions with render calls: {len(set(f for f, _ in hits))}")
    print()
    for (fn_addr, stub), sites in sorted(hits.items()):
        nid = stub_nid[stub]
        lib = nid_lib.get(nid, "?")
        fn = f"0x{fn_addr:08X}" if fn_addr is not None else "<none>"
        locs = ", ".join(f"{f}:{l}" for f, l in sites)
        print(f"  fn={fn:12s} -> {lib:14s} NID=0x{nid:08X} stub=0x{stub:08X}  ({locs})")

    called = {stub for _, stub in hits}
    print()
    print("Stubs with NO call site:")
    for addr in sorted(set(wanted) - called):
        nid = stub_nid[addr]
        print(f"  0x{addr:08X}  {nid_lib.get(nid, '?')} NID=0x{nid:08X}")


if __name__ == "__main__":
    main()
