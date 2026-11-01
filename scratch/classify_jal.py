#!/usr/bin/env python3
"""Classify jal sites in the Allegrex ELF: is the fall-through a tail call?

A genuine guest tail call has the shape:
    jal  target
    <delay slot>
    lw   $ra, N($sp)     # restore the ORIGINAL return address
    jr   $ra             # return to the original caller
The host can compile that to `return psp_func_target();` and reuse the frame.

A jal followed by ordinary work (not a $ra restore + jr $ra) is a NORMAL call:
the host must set $ra = a+8, call, and continue at a+8.
"""
import struct
import sys

ELF = "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/scratch/extracted/PSP_GAME/SYSDIR/EBOOT.ELF"

data = open(ELF, "rb").read()

# --- ELF32 little-endian program headers ---
e_phoff = struct.unpack_from("<I", data, 0x1C)[0]
e_phentsize = struct.unpack_from("<H", data, 0x2A)[0]
e_phnum = struct.unpack_from("<H", data, 0x2C)[0]

segments = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
        struct.unpack_from("<IIIIIIII", data, off)
    if p_type == 1:  # PT_LOAD
        segments.append((p_vaddr, p_offset, p_filesz))
        print("LOAD vaddr=0x%08X offset=0x%X filesz=0x%X flags=0x%X"
              % (p_vaddr, p_offset, p_filesz, p_flags))


def word(vaddr):
    for va, off, size in segments:
        if va <= vaddr and vaddr + 4 <= va + size:
            return struct.unpack_from("<I", data, off + (vaddr - va))[0]
    return None


def is_jal(w):
    return w is not None and (w >> 26) == 3


def jal_target(w, pc):
    return ((pc + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


def disasm_one(pc):
    """Return (mnemonic, operand) for the instruction at pc."""
    w = word(pc)
    if w is None:
        return ("???", "")
    op = w >> 26
    rs = (w >> 21) & 0x1F
    rt = (w >> 16) & 0x1F
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op == 0:
        funct = w & 0x3F
        rd = (w >> 11) & 0x1F
        if funct == 0x08:   # jr
            return ("jr", "$%d" % rs)
        if funct == 0x09:   # jalr
            return ("jalr", "$%d" % rs)
        if funct == 0x21:   # addu
            return ("addu", "$%d,$%d,$%d" % (rd, rs, rt))
        return ("op%02x" % funct, "")
    if op == 0x23:  # lw
        return ("lw", "$%d,%d($%d)" % (rt, simm, rs))
    if op == 0x2B:  # sw
        return ("sw", "$%d,%d($%d)" % (rt, simm, rs))
    if op == 0x0F:  # lui
        return ("lui", "$%d,0x%X" % (rt, imm))
    if op == 0x09:  # addiu
        return ("addiu", "$%d,$%d,%d" % (rt, rs, simm))
    if op == 0x0D:  # ori
        return ("ori", "$%d,$%d,0x%X" % (rt, rs, imm))
    if op == 0x05:  # bne
        return ("bne", "")
    if op == 0x04:  # beq
        return ("beq", "")
    if op == 0x02:  # j
        return ("j", "0x%08X" % ((pc + 4) & 0xF0000000 | (w & 0x03FFFFFF) << 2))
    if op == 0x03:  # jal
        return ("jal", "0x%08X" % jal_target(w, pc))
    return ("op%02x" % op, "")


def classify(pc):
    """Is the jal at pc a guest tail call?

    Look at the instruction at pc+8 (the callee's return point):
      - jr $ra  with the $ra restored by an lw from the stack just before
        -> genuine tail call
    Also accept the simple form where pc+8 is `jr $ra` directly and the
    preceding delay slot restored $ra.
    """
    m8, o8 = disasm_one(pc + 8)
    if m8 == "jr" and o8 == "$31":
        return True, "jr $ra at +8"
    # lw $ra, N($sp) followed by jr $ra at +12
    m8, o8 = disasm_one(pc + 8)
    m12, o12 = disasm_one(pc + 12)
    if m8 == "lw" and o8.startswith("$31,") and m12 == "jr" and o12 == "$31":
        return True, "lw $ra + jr $ra"
    return False, "normal call"


print()
print("=== jal sites classified ===")
stats = {"tail": 0, "normal": 0}
examples = []
for va, off, size in segments:
    if not (va == 0x00000000):   # the executable .text segment
        continue
    for a in range(va, va + size - 4, 4):
        w = word(a)
        if not is_jal(w):
            continue
        tgt = jal_target(w, a)
        is_tail, why = classify(a)
        stats["tail" if is_tail else "normal"] += 1
        if len(examples) < 40:
            examples.append((a, tgt, is_tail, why))

for a, tgt, is_tail, why in examples:
    print("0x%08X jal 0x%08X  %-8s  %s" % (a, tgt, "TAIL" if is_tail else "normal", why))

print()
print("TOTAL jal sites: %d  tail=%d  normal=%d"
      % (stats["tail"] + stats["normal"], stats["tail"], stats["normal"]))
