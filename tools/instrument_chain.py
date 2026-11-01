#!/usr/bin/env python3
"""Inject psp_chain_hit() calls into selected generated functions.

The boot-to-GE call chain is long and mostly unreachable until one specific
link works. Counting every function entry through PSPRECOMP_TRACE rebuilds
34k functions and slows the run enough to hide the answer, so instead this
tool patches the handful of generated functions on the chain, adding one
counter call each.

Idempotent: a function that already has the marker is skipped.

Usage:
    instrument_chain.py --generated DIR --addresses 0x5A4,0x6AC,...
    instrument_chain.py --generated DIR --addresses ... --dry-run
    instrument_chain.py --generated DIR --addresses ... --revert
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

MARKER = "psp_chain_hit("


def find_function_files(generated: Path, addr: int) -> list[Path]:
    """Files that define psp_func_<addr> or contain the label L_<addr>.

    Some chain links are interior labels rather than functions of their own
    (0x6AC is a branch target inside psp_body_000005A4). Matching only the
    function name would skip those entirely, and the chain would look broken
    at a link that actually works.
    """
    fname = re.compile(rf"\bpsp_func_{addr:08X}\b")
    lbl = re.compile(rf"^L_{addr:08X}:", re.M)
    hits = []
    for path in sorted(generated.glob("mc3_funcs_*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if fname.search(text) or lbl.search(text):
            hits.append(path)
    return hits


def instrument(path: Path, addr: int, dry_run: bool) -> tuple[int, int]:
    """Insert a chain_hit call for addr.

    Two shapes need handling. Most chain links are their own function, so the
    call goes at the top of psp_body_<addr>. Some are interior labels of a
    larger function (0x6AC is a branch target inside psp_body_000005A4), and
    those need the call at the label itself -- counting the enclosing body
    would say "reached" for a target that was never actually entered.

    Returns (functions_patched, functions_skipped).
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    body = f"psp_body_{addr:08X}"
    label = f"L_{addr:08X}:"

    patched = skipped = 0
    out = []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)

        # Interior label: L_XXXXXXXX: PSP_MARK(...)
        if line.strip().startswith(label):
            out.append(f"    {MARKER}0x{addr:08X}u);")
            patched += 1
            i += 1
            continue

        if body in line and "uint32_t _entry" in line:
            j = i + 1
            inserted = False
            while j < len(lines) and j < i + 6:
                nxt = lines[j]
                if MARKER in nxt:
                    inserted = True
                    break
                out.append(nxt)
                j += 1
                if "PSP_SP_ENTER" in nxt or "switch (_entry)" in nxt:
                    call = f"    {MARKER}0x{addr:08X}u);"
                    if not dry_run:
                        out.append(call)
                    inserted = True
                    patched += 1
                    break
            if not inserted and not patched:
                skipped += 1
            i = j
            continue
        i += 1

    if patched and not dry_run:
        path.write_text("\n".join(out), encoding="utf-8")
    return patched, skipped


def revert(path: Path, addr: int) -> int:
    """Remove previously injected chain_hit calls for this address."""
    text = path.read_text(encoding="utf-8", errors="replace")
    pat = re.compile(rf"^\s*{re.escape(MARKER)}0x{addr:08X}u\);\n", re.M)
    new_text, n = pat.subn("", text)
    if n:
        path.write_text(new_text, encoding="utf-8")
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--generated", required=True, type=Path,
                    help="directory holding mc3_funcs_*.c")
    ap.add_argument("--addresses", required=True,
                    help="comma-separated hex addresses to instrument")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--revert", action="store_true")
    args = ap.parse_args()

    if not args.generated.is_dir():
        print(f"error: {args.generated} is not a directory", file=sys.stderr)
        return 2

    addrs = []
    for tok in args.addresses.split(","):
        tok = tok.strip()
        if not tok:
            continue
        addrs.append(int(tok, 16))

    total_patched = total_reverted = total_skipped = 0
    for addr in addrs:
        files = find_function_files(args.generated, addr)
        if not files:
            print(f"  {addr:08X}: no generated function found")
            continue
        for path in files:
            if args.revert:
                n = revert(path, addr)
                total_reverted += n
                print(f"  {addr:08X}: {path.name}: reverted {n}")
            else:
                p, s = instrument(path, addr, args.dry_run)
                total_patched += p
                total_skipped += s
                print(f"  {addr:08X}: {path.name}: patched {p} skipped {s}")

    verb = "reverted" if args.revert else "patched"
    print(f"total {verb}: {total_reverted if args.revert else total_patched}"
          f" (skipped {total_skipped})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
