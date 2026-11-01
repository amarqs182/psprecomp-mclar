#!/usr/bin/env python3
"""Identify a PSP NID by computing the SHA-1 based NID of candidate names.

PSP import NIDs are derived from the function name. Verify the algorithm
against known pairs taken from psprecomp's HLE registrations, then search a
candidate list for the target NID.

Usage:
  nid_lookup.py <target-nid> [candidate-file]
"""
import hashlib
import sys


def nid_be(name: str) -> int:
    """First 4 bytes of SHA-1, big-endian."""
    h = hashlib.sha1(name.encode("ascii")).digest()
    return int.from_bytes(h[:4], "big")


def nid_le(name: str) -> int:
    """First 4 bytes of SHA-1, little-endian."""
    h = hashlib.sha1(name.encode("ascii")).digest()
    return int.from_bytes(h[:4], "little")


# Known pairs from src/psprecomp/src/hle/sysmem.c
KNOWN = {
    0x237DBD4F: "sceKernelAllocPartitionMemory",
    0xB6D61D02: "sceKernelFreePartitionMemory",
    0x9D9A5BA1: "sceKernelGetBlockHeadAddr",
    0x7591C7DB: "sceKernelSetCompiledSdkVersion",
    0xF77D77CB: "sceKernelSetCompilerVersion",
    0x13A5ABEF: "sceKernelPrintf",
    0xA291F107: "sceKernelMaxFreeMemSize",
}

# From the project's other HLE registrations.
KNOWN.update({
    0x3A622550: "sceCtrlPeekBufferPositive",
    0xAB49E76A: "sceGeListEnqueue",
    0x289D82FE: "sceDisplaySetFrameBuf",
    0x36CDFADE: "sceDisplayWaitVblank",
    0x446D8DE6: "sceKernelCreateThread",
    0xF475845D: "sceKernelStartThread",
    0xEA748E31: "sceKernelChangeCurrentThreadAttr",
})


def detect_algorithm():
    """Return the function that reproduces every known NID, or None."""
    for fn, label in ((nid_be, "big-endian"), (nid_le, "little-endian")):
        if all(fn(name) == nid for nid, name in KNOWN.items()):
            return fn, label
    return None


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    target = int(sys.argv[1], 16)
    detected = detect_algorithm()
    if detected is None:
        print("ERROR: no known NID matched either byte order; algorithm unknown")
        for nid, name in KNOWN.items():
            print(f"  {nid:#010x} {name}: be={nid_be(name):#010x} le={nid_le(name):#010x}")
        return 1

    nid_fn, order = detected
    print(f"algorithm: first 4 bytes of SHA-1, {order}")
    print(f"target:    {target:#010x}\n")

    candidates: list[str] = []
    if len(sys.argv) > 2:
        with open(sys.argv[2], "r", encoding="utf-8") as fh:
            candidates = [line.strip() for line in fh if line.strip()]
    else:
        candidates = list(KNOWN.values())

    hits = [c for c in candidates if nid_fn(c) == target]
    if hits:
        print("MATCH:")
        for h in hits:
            print(f"  {h}")
    else:
        print(f"no match in {len(candidates)} candidates")
    return 0 if hits else 1


if __name__ == "__main__":
    sys.exit(main())
