#!/usr/bin/env python3
"""Refined tail-call classification for the Allegrex ELF.

Two MIPS shapes are genuine tail calls:

  (A) the DELAY SLOT restores $ra, and the fall-through jumps through it:
        jal   target          # a
        lw    $ra, N($sp)     # a+4 -- delay slot: clobbers the link
        jr    $ra             # a+8 -- return to the ORIGINAL caller
      The callee returns to a+8, but $ra no longer holds a+8, so it
      returns to our caller instead. Nothing of ours runs afterwards.

  (B) the fall-through restores $ra and then jumps through it:
        jal   target          # a
        nop                   # a+4
        lw    $ra, N($sp)     # a+8
        jr    $ra             # a+12

Anything else continues in this function and is a regular call.
"""
import struct
import collections

ELF = "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/scratch/extracted/PSP_GAME/SYSDIR/EBOOT.ELF"
data = open(ELF, "rb").read()

e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
e_phnum = struct.unpack_from("<H", data, 0x2C)[0]
segments = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<IIIII", data, off)
    if p_type == 1:
        segments.append((p_vaddr, p_offset, p_filesz))


def word(va):
    for v, o, s in segments:
        if v <= va and va + 4 <= v + s:
            return struct.unpack_from("<I", data, o + (va - v))[0]
    return None


def decode(va):
    """Return (op, rd, rs, rt, is_return) for the instruction at va."""
    w = word(va)
    if w is None:
        return None
    op = w >> 26
    rs = (w >> 21) & 0x1F
    rt = (w >> 16) & 0x1F
    rd = (w >> 11) & 0x1F
    funct = w & 0x3F
    if op == 0 and funct == 0x08:          # jr
        return ("JR", rd, rs, rt, rs == 31)
    if op == 0 and funct == 0x21:          # addu
        return ("ADDU", rd, rs, rt, False)
    if op == 0x23:                         # lw
        return ("LW", rd, rs, rt, False)
    if op == 0x2B:                         # sw
        return ("SW", rd, rs, rt, False)
    if op == 0x03:                         # jal
        tgt = ((va + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        return ("JAL", rd, rs, rt, tgt)
    return ("OP%02x" % op, rd, rs, rt, False)


def is_lw_ra(va):
    d = decode(va)
    return d is not None and d[0] == "LW" and d[3] == 31      # rt == $ra


def is_jr_ra(va):
    d = decode(va)
    return d is not None and d[0] == "JR" and d[4]            # jr $ra


def classify(a):
    d = decode(a)
    if d is None or d[0] != "JAL":
        return None, ""
    # (A) delay slot restores $ra, fall-through is jr $ra
    if is_lw_ra(a + 4) and is_jr_ra(a + 8):
        return "A", "delay-slot lw $ra + jr $ra"
    # (B) fall-through restores $ra, then jr $ra
    if is_lw_ra(a + 8) and is_jr_ra(a + 12):
        return "B", "lw $ra + jr $ra"
    return "normal", ""


counts = collections.Counter()
examples = collections.defaultdict(list)
targets = collections.Counter()

va0, off0, size0 = segments[0]
for a in range(va0, va0 + size0 - 16, 4):
    kind, why = classify(a)
    if kind is None:
        continue
    counts[kind] += 1
    tgt = decode(a)[4]
    targets[tgt] += 1
    if len(examples[kind]) < 8:
        examples[kind].append((a, tgt, why))

print("=== classification ===")
for k, v in counts.items():
    print("  %-8s %d" % (k, v))
print()
for k, lst in examples.items():
    print("--- %s ---" % k)
    for a, tgt, why in lst:
        print("  0x%08X jal 0x%08X   %s" % (a, tgt, why))
    print()

# Which tail-call targets are the mutual recursion pair?
print("=== top tail-call targets ===")
for tgt, n in targets.most_common(15):
    if n > 20:
        print("  0x%08X  %d" % (tgt, n))
