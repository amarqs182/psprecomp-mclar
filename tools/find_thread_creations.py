#!/usr/bin/env python3
"""Find sceKernelCreateThread / sceKernelStartThread call sites and the entry
address each one passes.

The game creates its own threads; the host has to know which entry points the
game expects to run. This scans the generated C for calls to the
sceKernelCreateThread import stub and prints the arguments set before it.

Usage:
    python3 tools/find_thread_creations.py [--dir scratch/generated]
"""
import argparse
import os
import re

# NID of sceKernelCreateThread is 0x3F53E640 -> stub 0x003C4D60
CREATE_STUB = "003C4D60"
# sceKernelStartThread NID 0x58B1F937 -> stub 0x003C4D70
START_STUB = "003C4D70"

BODY_RE = re.compile(r"^static void psp_body_([0-9A-Fa-f]+)\(")
# r_a1 = 0xXXXXXXXXu;   <- the entry address argument
A1_RE = re.compile(r"r_a1\s*=\s*0x([0-9A-Fa-f]{1,8})u;")
A1_MOVE_RE = re.compile(r"r_a1\s*=\s*(r_a0|r_a2|r_zero|r_s\d)\s*;")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="scratch/generated")
    args = ap.parse_args()

    for stub, label in ((CREATE_STUB, "sceKernelCreateThread"),
                        (START_STUB, "sceKernelStartThread")):
        print(f"=== {label} (stub 0x{stub}) call sites ===")
        total = 0
        for name in sorted(os.listdir(args.dir)):
            if not (name.startswith("mc3_funcs_") and name.endswith(".c")):
                continue
            path = os.path.join(args.dir, name)
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()

            current = None
            for i, line in enumerate(lines):
                bm = BODY_RE.match(line)
                if bm:
                    current = int(bm.group(1), 16)
                if f"psp_import_{stub}(" not in line:
                    continue
                total += 1
                fn = f"0x{current:08X}" if current is not None else "<none>"
                # look back up to 40 lines for r_a1 assignment (the entry arg)
                entry = None
                for j in range(i - 1, max(-1, i - 41), -1):
                    am = A1_RE.search(lines[j])
                    if am:
                        entry = "0x" + am.group(1).zfill(8)
                        break
                    mm = A1_MOVE_RE.search(lines[j])
                    if mm:
                        entry = f"<{mm.group(1)}>"
                        break
                print(f"  fn={fn}  {name}:{i+1}  entry={entry}")
        print(f"  total: {total}")
        print()


if __name__ == "__main__":
    main()
