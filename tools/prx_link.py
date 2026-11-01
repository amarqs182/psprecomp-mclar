#!/usr/bin/env python3
"""Analyse PSP PRX relocations and apply them, producing a linked ELF.

A PSP module is a PRX: the linker leaves every address that crosses a section
boundary as a zero placeholder in the instruction stream and records the fixup
in a SHT_PRXRELOC section (0x700000A0) -- a type readelf silently skips.  The
kernel's module loader resolves those at load time.  A static recompiler has
to resolve them at *recompile* time instead, otherwise every cross-section
reference in the game reads through address 0 and the module never gets past
its own startup code.

Two fields matter in a PRX relocation entry:

    r_info & 0xFF        the MIPS relocation type
    (r_info >> 16) & 0xFFFF   the *segment* the target lives in

There is no symbol table to consult -- a PRX has a single empty symbol -- so
the target is reconstructed from the instruction pair itself plus the segment
base:

    value  = (hi_imm << 16) + (int16)lo_imm     the linker-time addend
    target = segment_base[seg] + value

and then written back as

    hi_imm = (target + 0x8000) >> 16
    lo_imm = target & 0xFFFF

The +0x8000 rounding is not cosmetic: addiu sign-extends its immediate, so a
low half of 0x8000 or above borrows from the high half.

Usage:
    dump_prx_reloc.py EBOOT.ELF                  # summary
    dump_prx_reloc.py EBOOT.ELF --types          # per-type listing
    dump_prx_reloc.py EBOOT.ELF --link OUT.ELF   # write a linked copy
"""
import struct
import sys
from collections import Counter, defaultdict

SHT_REL = 9
SHT_PRXRELOC = 0x700000A0
SHT_SYMTAB = 2
SHT_NOBITS = 8

R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6

R_MIPS_NAMES = {
    0: "R_MIPS_NONE", 1: "R_MIPS_16", 2: "R_MIPS_32", 3: "R_MIPS_REL32",
    4: "R_MIPS_26", 5: "R_MIPS_HI16", 6: "R_MIPS_LO16", 7: "R_MIPS_GPREL16",
    8: "R_MIPS_LITERAL", 9: "R_MIPS_GOT16",
}


class Elf:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.d = bytearray(fh.read())
        d = self.d
        if d[:4] != b"\x7fELF":
            raise SystemExit("not an ELF")
        if d[4] != 1:
            raise SystemExit("only ELF32 supported")
        self.le = d[5] == 1
        self.end = "<" if self.le else ">"
        (self.e_type, self.e_machine, self.e_version, self.e_entry,
         self.e_phoff, self.e_shoff, self.e_flags, self.e_ehsize,
         self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from(self.end + "HHIIIIIHHHHHH", d, 16)

        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, stype, flags, addr, offset, size, link, info,
             align, entsize) = struct.unpack_from(self.end + "IIIIIIIIII", d, off)
            self.sections.append(dict(
                idx=i, name_off=name, type=stype, flags=flags, addr=addr,
                offset=offset, size=size, link=link, info=info,
                align=align, entsize=entsize))

        shstr = self.sections[self.e_shstrndx]
        self.shstrtab = bytes(d[shstr["offset"]:shstr["offset"] + shstr["size"]])
        for s in self.sections:
            s["name"] = self._cstr(self.shstrtab, s["name_off"])

        # Program headers -> segment bases.  A PRX has two: code+rodata+data,
        # then sdata/sbss/bss.  The relocation's segment field indexes these.
        self.segments = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            (ptype, poff, vaddr, paddr, filesz, memsz, flags,
             align) = struct.unpack_from(self.end + "IIIIIIII", d, off)
            if ptype == 1:  # PT_LOAD
                self.segments.append(dict(vaddr=vaddr, filesz=filesz,
                                          memsz=memsz, offset=poff))

    @staticmethod
    def _cstr(tab, off):
        end = tab.find(b"\x00", off)
        return tab[off:end].decode("utf-8", "replace")

    def reloc_sections(self):
        return [s for s in self.sections
                if s["type"] in (SHT_REL, SHT_PRXRELOC)]

    def target_section(self, rs):
        """The section a relocation section applies to."""
        if rs["info"] < len(self.sections):
            return self.sections[rs["info"]]
        return None

    def vaddr_to_file(self, vaddr):
        for s in self.sections:
            if s["type"] == SHT_NOBITS:
                continue
            if s["addr"] <= vaddr < s["addr"] + s["size"]:
                return s["offset"] + (vaddr - s["addr"])
        return None

    def section_of(self, vaddr):
        for s in self.sections:
            if s["size"] and s["addr"] <= vaddr < s["addr"] + s["size"]:
                return s["name"]
        return None

    def seg_base(self, seg):
        if seg < len(self.segments):
            return self.segments[seg]["vaddr"]
        return None


def read_entries(elf, rs):
    """Yield (r_offset, rtype, seg) for one relocation section."""
    n = rs["size"] // 8
    for i in range(n):
        off = rs["offset"] + i * 8
        r_offset, r_info = struct.unpack_from(elf.end + "II", elf.d, off)
        yield r_offset, (r_info & 0xFF), ((r_info >> 16) & 0xFFFF)


def resolve_offsets(elf, rs):
    """Decide whether r_offset is absolute or section-relative.

    The two agree for .text (linked at 0) and diverge for the data sections,
    so the answer is derived from the data rather than assumed: whichever
    interpretation keeps the offsets inside the target section wins.
    """
    tgt = elf.target_section(rs)
    if tgt is None:
        return "absolute"
    n = rs["size"] // 8
    abs_ok = rel_ok = 0
    for i in range(n):
        off = rs["offset"] + i * 8
        r_offset = struct.unpack_from(elf.end + "I", elf.d, off)[0]
        if tgt["addr"] <= r_offset < tgt["addr"] + tgt["size"]:
            abs_ok += 1
        if r_offset < tgt["size"]:
            rel_ok += 1
    if rel_ok > abs_ok:
        return "relative"
    return "absolute"


def apply_relocations(elf, verbose=False):
    """Patch the in-memory image.  Returns a stats dict."""
    stats = Counter()
    patched = []

    for rs in elf.reloc_sections():
        tgt = elf.target_section(rs)
        if tgt is None:
            continue
        mode = resolve_offsets(elf, rs)
        seg_base = [elf.seg_base(i) for i in range(len(elf.segments))]

        entries = list(read_entries(elf, rs))

        if mode == "relative":
            def file_off(r_offset):
                fo = tgt["offset"] + r_offset
                return fo if fo + 4 <= len(elf.d) else None
        else:
            def file_off(r_offset):
                fo = elf.vaddr_to_file(r_offset)
                return fo if fo is not None and fo + 4 <= len(elf.d) else None

        # HI16/LO16 pairs: resolve the address and write both halves back.
        #
        # A LO16 belongs to the most recent unmatched HI16 *in the same
        # segment* -- the two are not necessarily adjacent (the LO16 often
        # lands in a delay slot, several instructions later), but the linker
        # always emits the HI16 first, so a pending stack pairs them
        # unambiguously.
        pending = {}
        for r_offset, rtype, seg in entries:
            if rtype == R_MIPS_HI16:
                pending.setdefault(seg, []).append(r_offset)
                continue
            if rtype != R_MIPS_LO16:
                continue
            stack = pending.get(seg)
            if not stack:
                stats["LO16_unpaired"] += 1
                continue
            hi_offset = stack.pop()
            hi_fo = file_off(hi_offset)
            lo_fo = file_off(r_offset)
            if hi_fo is None or lo_fo is None:
                stats["HI16_unmapped"] += 1
                continue
            hi_word = struct.unpack_from(elf.end + "I", elf.d, hi_fo)[0]
            lo_word = struct.unpack_from(elf.end + "I", elf.d, lo_fo)[0]
            hi_imm = hi_word & 0xFFFF
            lo_imm = lo_word & 0xFFFF
            value = (hi_imm << 16) + ((lo_imm ^ 0x8000) - 0x8000)
            base = seg_base[seg] if seg < len(seg_base) else 0
            target = (base + value) & 0xFFFFFFFF
            new_hi = ((target + 0x8000) >> 16) & 0xFFFF
            new_lo = target & 0xFFFF
            struct.pack_into(elf.end + "I", elf.d, hi_fo,
                             (hi_word & 0xFFFF0000) | new_hi)
            struct.pack_into(elf.end + "I", elf.d, lo_fo,
                             (lo_word & 0xFFFF0000) | new_lo)
            stats["HI16"] += 1
            patched.append((hi_offset, target, elf.section_of(target)))

        for r_offset, rtype, seg in entries:
            if rtype == R_MIPS_32:
                fo = file_off(r_offset)
                if fo is not None:
                    stored = struct.unpack_from(elf.end + "I", elf.d, fo)[0]
                    base = seg_base[seg] if seg < len(seg_base) else 0
                    target = (base + stored) & 0xFFFFFFFF
                    if target != stored:
                        struct.pack_into(elf.end + "I", elf.d, fo, target)
                        stats["R_MIPS_32_changed"] += 1
                    stats["R_MIPS_32"] += 1
            elif rtype == R_MIPS_26:
                stats["R_MIPS_26"] += 1
            else:
                stats["other_%d" % rtype] += 1

    return stats, patched


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    elf = Elf(path)

    print(f"ELF type=0x{elf.e_type:x} entry=0x{elf.e_entry:08x} "
          f"segments={len(elf.segments)}")
    for i, sg in enumerate(elf.segments):
        print(f"  segment {i}: vaddr=0x{sg['vaddr']:08x} "
              f"filesz=0x{sg['filesz']:x} memsz=0x{sg['memsz']:x}")

    if "--link" in args:
        out = args[args.index("--link") + 1]
        stats, patched = apply_relocations(elf)
        with open(out, "wb") as fh:
            fh.write(elf.d)
        print(f"\nwrote {out}")
        for k, v in sorted(stats.items()):
            print(f"  {k}: {v}")
        # show where HI16 pairs landed
        bysec = Counter(p[2] for p in patched if p[2])
        print("  HI16 targets by section:", dict(bysec))
        return

    # summary mode
    total = Counter()
    for rs in elf.reloc_sections():
        mode = resolve_offsets(elf, rs)
        tgt = elf.target_section(rs)
        segs = Counter()
        n = rs["size"] // 8
        for r_offset, rtype, seg in read_entries(elf, rs):
            total[rtype] += 1
            segs[seg] += 1
        print(f"\n-- {rs['name']:24s} entries={n:6d} mode={mode:8s} "
              f"target={tgt['name'] if tgt else '?'} "
              f"addr=0x{tgt['addr']:08x}" if tgt else "")
        print(f"   segments: {dict(segs)}")

    print(f"\n=== totals ===")
    for t, c in sorted(total.items()):
        print(f"  {R_MIPS_NAMES.get(t, 'type%d' % t):18s} {c}")


if __name__ == "__main__":
    main()
