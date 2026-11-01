#!/usr/bin/env python3
"""Dump SHT_PRXRELOC relocations from a PSP PRX (readelf can't parse them).

A PSP module tags its relocation sections SHT_PRXRELOC (0x700000A0) instead
of the generic SHT_REL (9).  The entry layout is identical, but readelf skips
them, so a static analysis of the module sees every HI16/LO16 pair as a bare
zero and concludes the game reads through address 0.

Usage:  dump_prx_reloc.py EBOOT.ELF [--types] [--symbol NAME]
"""
import struct
import sys
from collections import Counter, defaultdict

SHT_REL = 9
SHT_PRXRELOC = 0x700000A0
SHT_SYMTAB = 2

R_MIPS_32 = 2
R_MIPS_26 = 4
R_MIPS_HI16 = 5
R_MIPS_LO16 = 6
R_MIPS_GPREL16 = 7
R_MIPS_GOT16 = 9
R_MIPS_REL32 = 3

R_MIPS_NAMES = {
    0: "R_MIPS_NONE",
    1: "R_MIPS_16",
    2: "R_MIPS_32",
    3: "R_MIPS_REL32",
    4: "R_MIPS_26",
    5: "R_MIPS_HI16",
    6: "R_MIPS_LO16",
    7: "R_MIPS_GPREL16",
    8: "R_MIPS_LITERAL",
    9: "R_MIPS_GOT16",
    11: "R_MIPS_GPREL32",
}


class Elf:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.d = fh.read()
        d = self.d
        if d[:4] != b"\x7fELF":
            raise SystemExit("not an ELF")
        self.is32 = d[4] == 1
        self.le = d[5] == 1
        if not self.is32:
            raise SystemExit("only ELF32 supported")
        end = "<" if self.le else ">"
        self.end = end
        (self.e_type, self.e_machine, self.e_version, self.e_entry,
         self.e_phoff, self.e_shoff, self.e_flags, self.e_ehsize,
         self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from(end + "HHIIIIIHHHHHH", d, 16)

        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (name, stype, flags, addr, offset, size, link, info,
             align, entsize) = struct.unpack_from(end + "IIIIIIIIII", d, off)
            self.sections.append(dict(
                idx=i, name_off=name, type=stype, flags=flags, addr=addr,
                offset=offset, size=size, link=link, info=info,
                align=align, entsize=entsize))

        # section header string table
        shstr = self.sections[self.e_shstrndx]
        self.shstrtab = d[shstr["offset"]:shstr["offset"] + shstr["size"]]
        for s in self.sections:
            s["name"] = self._cstr(self.shstrtab, s["name_off"])

        self.symbols = []
        self.symtab = []
        for s in self.sections:
            if s["type"] == SHT_SYMTAB:
                self.symtab.append(s)

    @staticmethod
    def _cstr(tab, off):
        end = tab.find(b"\x00", off)
        return tab[off:end].decode("utf-8", "replace")

    def load_symbols(self, symtab):
        d, end = self.d, self.end
        strtab = self.sections[symtab["link"]]
        strdata = d[strtab["offset"]:strtab["offset"] + strtab["size"]]
        n = symtab["size"] // 16
        out = []
        for i in range(n):
            off = symtab["offset"] + i * 16
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from(end + "IIIBBH", d, off)
            out.append(dict(
                name=self._cstr(strdata, st_name), value=st_value,
                size=st_size, info=st_info, other=st_other, shndx=st_shndx))
        return out

    def reloc_sections(self):
        return [s for s in self.sections
                if s["type"] in (SHT_REL, SHT_PRXRELOC)]


def main():
    path = sys.argv[1]
    elf = Elf(path)
    print(f"ELF type=0x{elf.e_type:x} machine={elf.e_machine} "
          f"entry=0x{elf.e_entry:08x} shnum={elf.e_shnum}")
    print(f"sections={len(elf.sections)} symtabs={len(elf.symtab)}")

    relsecs = elf.reloc_sections()
    if not relsecs:
        print("no relocation sections")
        return

    total = 0
    type_counter = Counter()
    by_type = defaultdict(list)

    for rs in relsecs:
        symtab = elf.sections[rs["link"]] if rs["link"] < len(elf.sections) else None
        syms = elf.load_symbols(symtab) if symtab else []
        n = rs["size"] // 8
        print(f"\n-- reloc section {rs['name']} (idx {rs['idx']}) "
              f"type=0x{rs['type']:x} entries={n} symtab={symtab['name'] if symtab else '-'}")
        for i in range(n):
            off = rs["offset"] + i * 8
            r_offset, r_info = struct.unpack_from(elf.end + "II", elf.d, off)
            rtype = r_info & 0xFF
            rsym = r_info >> 8
            sym = syms[rsym] if rsym < len(syms) else dict(
                name="<bad>", value=0, shndx=0)
            total += 1
            type_counter[rtype] += 1
            by_type[rtype].append((r_offset, sym["name"], sym["value"],
                                   sym["shndx"]))
        if i == 0:
            print(f"   sample: off=0x{r_offset:08x} type={R_MIPS_NAMES.get(rtype, rtype)} "
                  f"sym={sym['name']} value=0x{sym['value']:08x} shndx={sym['shndx']}")

    print(f"\n=== {total} relocations total ===")
    for t, c in sorted(type_counter.items()):
        print(f"  {R_MIPS_NAMES.get(t, 'type%d' % t):18s} {c}")

    if "--types" in sys.argv:
        for t in sorted(by_type):
            print(f"\n--- {R_MIPS_NAMES.get(t, t)} ({len(by_type[t])}) ---")
            for r_offset, name, value, shndx in by_type[t][:40]:
                print(f"  off=0x{r_offset:08x} sym={name} val=0x{value:08x} shndx={shndx}")

    if "--symbol" in sys.argv:
        want = sys.argv[sys.argv.index("--symbol") + 1]
        print(f"\n=== relocations touching symbol {want} ===")
        for t, items in by_type.items():
            for r_offset, name, value, shndx in items:
                if name == want:
                    print(f"  off=0x{r_offset:08x} type={R_MIPS_NAMES.get(t, t)} "
                          f"val=0x{value:08x} shndx={shndx}")


if __name__ == "__main__":
    main()
