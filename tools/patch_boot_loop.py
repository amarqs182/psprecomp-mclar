#!/usr/bin/env python3
"""Re-apply the boot-loop patches to the regenerated mc3_funcs_000.c.

The Allegrex emitter's tail-call detection was wrong for a long time: it
treated *every* call whose fall-through stayed inside the function as a tail
call, so each body returned at its first call. Fixing that in the emitter
changed the boot path's control flow, and these patches -- written against
the OLD emission -- may no longer apply. This script re-applies them
idempotently and reports which ones actually took.

Run it after every re-emit of scratch/generated/.
"""
import re
import sys

GEN = "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/scratch/generated/mc3_funcs_000.c"

# (name, anchor, replacement) -- each anchor must be unique in the file.
PATCHES = [
    (
        "boot-a0-16",
        "    /* 00000148  lw         $a0, 24($sp) */\n"
        "    r_a0 = psp_read32(r_sp + 24);\n",
        "    /* 00000148  lw         $a0, 24($sp) */\n"
        "    /* PATCH: force a0 = 16 so the boot path proceeds past its\n"
        "     * polling loop (sp[24] was zero in the emulated environment). */\n"
        "    r_a0 = 16u;\n",
    ),
    (
        "boot-s5-64",
        "    /* 000000FC  addiu      $s5, $zero, -1 */\n"
        "    r_s5 = r_zero + -1;\n",
        "    /* 000000FC  addiu      $s5, $zero, -1 */\n"
        "    r_s5 = r_zero + -1;\n"
        "    /* PATCH: s5 = 64 matches sp[4], the value the boot path\n"
        "     * compares against when looking for its exit condition. */\n"
        "    r_s5 = 64u;\n",
    ),
    (
        "boot-s3-zero",
        "L_00000180: PSP_MARK(0x00000180u);\n"
        "    /* 00000180  bne        $s3, $zero, 0x000001C0 */\n",
        "L_00000180: PSP_MARK(0x00000180u);\n"
        "    /* 00000180  bne        $s3, $zero, 0x000001C0 */\n"
        "    /* PATCH: s3 = 0 -- the flag was clobbered by a firmware\n"
        "     * call, so the loop always took the branch back to 0x134. */\n"
        "    r_s3 = 0u;\n",
    ),
]


def main():
    with open(GEN, encoding="utf-8") as fh:
        src = fh.read()

    applied, skipped = [], []
    for name, anchor, repl in PATCHES:
        if repl in src:
            skipped.append(name + " (already applied)")
            continue
        if src.count(anchor) != 1:
            skipped.append(name + " (anchor matched %d times -- skipped)"
                           % src.count(anchor))
            continue
        src = src.replace(anchor, repl)
        applied.append(name)

    with open(GEN, "w", encoding="utf-8") as fh:
        fh.write(src)

    print("applied: %s" % (", ".join(applied) if applied else "(none)"))
    for s in skipped:
        print("skipped: %s" % s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
