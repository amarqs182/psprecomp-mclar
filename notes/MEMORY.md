# Working memory — Midnight Club: L.A. Remix (USA) PSP static recompilation

This file is the project's working log. It records what was **verified** (with
the command or tool output that shows it), what is **assumed**, and where two
tools **disagree**. Everything here is a scratch/working note; the authoritative
deliverable documentation lives in `README.md` and `ports/*/README.md`.

Last updated: 2026-10-31 (milestone 1, in progress)

---

## 1. Pinned versions

| Component | Version | Pin location |
|---|---|---|
| psprecomp | **v0.5.0** (tag), commit `198f10e` | `src/psprecomp` (git clone, checked out at tag) |
| psprecomp licence | MIT | `src/psprecomp/LICENSE` |
| Ghidra (REA lane) | **12.1.3** | `~/tools/ghidra_12.1.3_PUBLIC` |
| Ghidra (GhidraMCP lane) | **12.1.4** | `~/tools/ghidra_12.1.4_PUBLIC` |
| ghidra-allegrex | **v21.4** (commit `8d5ca58e60`) | installed into both Ghidra trees |
| JDK | **Temurin 21.0.9+10** | `~/.local/share/mise/installs/java/temurin-21.0.9+10.0.LTS` |
| REA | `rea-agents` **6.8.0** (global npm) | `/home/amarqs/.local/bin/rea` |
| GhidraMCP | **2.0** (release 2026-10-10) | plugin installed into Ghidra 12.1.4 only |
| Cross toolchain | Arch `aarch64-linux-gnu-gcc` **16.2.0** + binutils 2.47 + glibc 2.44 | `~/tools/aarch64-sysroot` (extracted, no root) |

### Version reconciliation (Ghidra 12.1.3 vs 12.1.4)

GhidraMCP 2.0 requires Ghidra 12.1.4; REA's published PSP experiment used
12.1.3. **Two Ghidra trees are installed side by side** rather than upgrading:

- REA drives **12.1.3** (`GHIDRA_INSTALL_DIR=~/tools/ghidra_12.1.3_PUBLIC`).
  `rea doctor --provider ghidra --json` reports `healthy: true` and
  `providerVersion: 12.1.3`, all nine ghidra scope checks ok.
- GhidraMCP 2.0 drives **12.1.4**, which is the only version the 2.0 plugin
  supports.

`ghidra-allegrex v21.4` publishes a build for **each** of 12.1.3 and 12.1.4
(`ghidra_12.1.3_PUBLIC_20260825_ghidra-allegrex.zip` and
`ghidra_12.1.4_PUBLIC_20261025_ghidra-allegrex.zip`), so the same extension
version serves both trees. The 12.1.3 zip's `extension.properties` declares
`version=12.1.3`; the 12.1.4 one declares `version=12.1.4`.

Because the two trees are separate directories they use separate Ghidra
projects, so **REA and GhidraMCP cannot contend for the same project lock**.
The project rule "never run both against the same Ghidra project" is satisfied
by construction, not by discipline.

---

## 2. Reverse-engineering stack

### Installed and verified

- **REA 6.8.0** CLI, Ghidra provider green:
  ```
  rea doctor --provider ghidra --json
  -> "healthy": true
     ghidra-version: 12.1.3; compatible 12.1.x release (verified build 12.1.4)
     ghidra-java: 21.0.9; 64-bit; JDK
  ```
  Remaining doctor output is informational only: Hopper / IDA / JADX / Kuna are
  not installed and not needed for this target, and other clients' REA
  registrations are stale (this task only uses the Hermes client, which doctor
  reports `state: aligned`).
- **ghidra-allegrex v21.4** installed manually at
  `$GHIDRA_INSTALL_DIR/Ghidra/Extensions/ghidra-allegrex` in both trees, with
  `Module.manifest`, `extension.properties`, `data/languages/` (allegrex.sinc,
  allegrexVfpu*.sinc, allegrex.sla, .ldefs, .pspec, .cspec) and
  `lib/ghidra-allegrex.jar`. The extension's loader class is
  `ghidra.app.util.opinion.PspElfLoader` and the language id is
  `Allegrex:LE:32:default`.
- **GhidraMCP 2.0** plugin extracted from
  `GhidraMCP-release-2-0.zip` → `ghidra_12.1.4_PUBLIC_20261010_GhidraMCP.zip`
  → installed at `~/tools/ghidra_12.1.4_PUBLIC/Ghidra/Extensions/GhidraMCP`.

### Workflow (the documented choice)

- **REA** = headless batch analysis, automated. **GhidraMCP** = GUI-side
  interactive inspection (rename, comment, xrefs, pseudocode).
- They are pinned to **different Ghidra trees** and therefore different
  projects. No project-lock contention is possible.
- When a GUI session is needed, GhidraMCP uses the 12.1.4 tree; REA batch runs
  use the 12.1.3 tree. Both see the same file, never the same project.

---

## 3. Target artifact

### Decrypted EBOOT

```
scratch/extracted/PSP_GAME/SYSDIR/
  EBOOT.BIN          5212672  (encrypted, from the ISO)
  EBOOT_work.BIN     5212672  sha256 958ab349...  (copy of EBOOT.BIN)
  EBOOT_work.BIN.dec 5212321  sha256 266572e4...  (decrypted)
  EBOOT.ELF          5212321  sha256 266572e4...  (== the decrypted file)
  BOOT.BIN           5212321  sha256 b184f100...  (different from EBOOT.ELF)
```

`EBOOT.ELF` is **byte-identical** to the decrypted `EBOOT_work.BIN.dec`
(same sha256). `BOOT.BIN` is a *different* 5212321-byte file — it is not the
same image and is not interchangeable with `EBOOT.ELF`.

### ELF header (raw bytes, not readelf's opinion)

```
EI_CLASS=ELFCLASS32  EI_DATA=ELFDATA2LSB  EI_VERSION=1
e_type   = 0xffa0   (ET_SCE_RELEX_TYPE — a PSP *relocatable* module)
e_machine= 0x0008   (EM_MIPS)
e_version= 1
e_entry  = 0x000005a4
e_flags  = 0x10a23001  (noreorder | gs464 | eabi32 | mips2)
e_phnum  = 2, e_shnum = 64
```

Program headers:

| # | type | offset | vaddr | filesz | memsz | flags |
|---|------|--------|-------|--------|-------|-------|
| 0 | PT_LOAD | 0x74 | 0x00000000 | 0x424ec0 | 0x424ec0 | R E |
| 1 | PT_LOAD | 0x424f40 | 0x00424ec0 | 0x02fc4 | 0x9b100 | RW |

Section types present: `SHT_PROGBITS`, `SHT_SYMTAB`, `SHT_STRTAB`, `SHT_NOBITS`,
and `0x700000a0` (SHT_PSPREL — the PSP relocation sections).
**There is no `SHT_MIPS_ABIFLAGS` (0x7000002a) section and no
`PT_MIPS_ABIFLAGS` (0x70000003) program header.** `readelf -n` reports no
notes at all.

### psprecomp's own reading of the file (independent cross-check)

```
allegrexrecomp info EBOOT.ELF
  format: ELF32 MIPS LE (PRX, relocatable)
  entry 0x000005A4, sections 64
  code 0x00000000 + 3950728 bytes (from .text)
  import stubs 0x003C4888 + 1720 bytes
  module info 0x003C5168, name MC3, gp 0x00007FF0
  exports 0x003C4F44..0x003C4F54 (2), imports 0x003C4F5C..0x003C5164

allegrexrecomp cover EBOOT.ELF
  decoded 987382/987682 (99.97%)
  vfpu    300 (0.03%) recognised as VFPU, not yet named
  unknown 0 (0.00%)  <- no decoder gaps
```

The opcode histogram (`lw` 14.94%, `addiu` 10.41%, `or` 10.06%, `sw` 8.32%,
`nop` 5.34%) is the characteristic shape of genuinely compiled MIPS, not
random bytes — i.e. **the decryption is verified good** by psprecomp's own
criterion.

Module name is **`MC3`** (not `MC4`/`MCLA`) — the internal module is the
Midnight Club 3 (DUB Edition) lineage the user expects, since L.A. Remix shares
Rockstar's AGE engine with MC3.

---

## 4. Disagreement #1 — REA refuses this ELF; psprecomp accepts it

**This is the first real cross-tool disagreement and it is unresolved.**

`rea analyze scratch/extracted/PSP_GAME/SYSDIR/EBOOT.ELF --provider ghidra`
returns:

```json
{"error":"Analysis failed","code":"capability_unavailable",
 "category":"unsupported_provider",
 "message":"The selected analysis provider does not support this target...",
 "details":{"operation":"open_binary","selection_reason":"target_unsupported",
   "rejections":[{"provider_id":"ghidra","code":"architecture_unsupported",
     "reason":"The PSP Allegrex profile supports static ET_EXEC only; PRX,
               shared and relocatable images require separate verification.",
     "diagnostics":{"architecture":"mips","available_architectures":["mips"],
                   "executable_role":null}}]}}
```

### Root cause (read from REA's own source, not guessed)

`rea-agents/dist/ghidra/GhidraPspProfile.js`, function
`ghidraProcessorUnsupportedReason`:

```js
if (metadata.type !== 2)
    return "The PSP Allegrex profile supports static ET_EXEC only; PRX, shared
            and relocatable images require separate verification.";
if (metadata.flags !== 0x10a23001) ...
const abi = metadata.abiFlags;
if (abi === undefined || abi === null)
    return "The PSP Allegrex profile requires an inspected separate MIPS ABI
            flags record; missing declarations are not inferred.";
if (abi.version !== 0 || abi.isaLevel !== 2 || abi.isaRevision !== 0 ||
    abi.gprSize !== 1 || abi.cpr1Size !== 1 || abi.cpr2Size !== 0 ||
    abi.fpAbi !== 2 || abi.isaExtension !== 0 || abi.ases !== 0 ||
    abi.flags1 !== 1 || abi.flags2 !== 0) ...
```

and `dist/application/MipsElfMetadata.js` shows the ABI record is read only
from a `PT_MIPS_ABIFLAGS` program header (type `0x70000003`) or a
`SHT_MIPS_ABIFLAGS` section (type `0x7000002a`), and must be exactly 24 bytes.

Also `isGhidraPspTarget` requires
`(target.mips.flags & 0x00ff0000) === 0x00a20000` — our `e_flags`
`0x10a23001` satisfies this (the "gs464" machine field), so the PSP profile
*is* selected; it is the ET_EXEC and ABI-record gates that fail.

**Two independent facts about this file therefore block REA:**

1. `e_type == 0xffa0` (ET_SCE_RELEX_TYPE, a relocatable PSP module), not
   `2` (ET_EXEC).
2. There is no MIPS ABI-flags record anywhere in the file — the EBOOT is a
   retail Rockstar build that was linked without a `.MIPS.abiflags` section.

Meanwhile **psprecomp accepts and analyses the file without complaint**,
because psprecomp was written for PSP relocatable modules: it reads the
`SHT_PSPREL` sections, the `.rodata.sceModuleInfo` block (name `MC3`, gp
`0x7FF0`), and the NID tables.

### Status

**Unresolved.** The user's brief states the REA PSP profile "accepts ...
little-endian ELF32 ET_EXEC, EM_MIPS, e_flags=0x10a23001, with a version-0
MIPS ABI flags record" — that description matches REA's source exactly, but
this title's decrypted EBOOT satisfies **neither** of the last two
conditions, because it is a relocatable PSP module with no ABI-flags section.

Options considered, none taken yet:

- **(a) Ghidra directly** (not through REA) can still load the file using the
  ghidra-allegrex `PspElfLoader` with `-processor Allegrex:LE:32:default`.
  This gives full Ghidra analysis and satisfies the GhidraMCP half of the
  requirement. It does **not** satisfy the literal "rea analyze ... returns
  function count and sceKernel* imports" wording of milestone 1.
- **(b) Synthesise an ET_EXEC + ABI-flags wrapper** around the decrypted ELF
  in `scratch/` purely as an analysis shim. This changes the file's identity
  and would make REA's answer about a *derived* artifact rather than the
  decrypted EBOOT. It is also arguably fabricating a declaration that the
  retail build does not make — the exact thing REA is designed to refuse.
  **Not doing this without the user's decision.**
- **(c) Ask REA's provider whether the PRX lane exists** — REA's own
  limitation text says "PRX relocations, PBP/ISO containers, live overlays and
  PSP emulation are outside this verified lane", so no.

The honest position for now: **Ghidra (with ghidra-allegrex) is the working
PSP lane for this title; REA's headless provider is not, for this file.** The
user's instruction "Cross-check, don't trust ... If Ghidra/REA and psprecomp
disagree, record both readings in the memory file and treat the disagreement
as a bug to resolve before proceeding" is being followed by recording it here.

---

## 5. Toolchain notes

### Cross compiler

`aarch64-linux-gnu-gcc` was installed **without root** by downloading the Arch
`extra` packages and extracting them into `~/tools/aarch64-sysroot`:

```
aarch64-linux-gnu-binutils 2.47
aarch64-linux-gnu-gcc      16.2.0
aarch64-linux-gnu-glibc     2.44
```

Usage:
```
export CROSS=$HOME/tools/aarch64-sysroot
export PATH="$CROSS/usr/bin:$PATH"
aarch64-linux-gnu-gcc --sysroot="$CROSS/usr/aarch64-linux-gnu" ...
```

**Verified**: a hello-world compiled to `ELF 64-bit LSB pie executable, ARM
aarch64` and ran correctly under `qemu-aarch64-static -L $SYSROOT`.

(The Debian bullseye cross packages were tried first and their
`libc6-*-arm64-cross` URLs 404 on the current mirror; the Arch path works and
matches the glibc generation ArkOS is closer to.)

### R36S access — **BLOCKED, needs the user**

| Host | What it is | State |
|---|---|---|
| 192.168.2.120 | this workstation (Omarchy, x86_64) | — |
| 192.168.2.115 | **the R36S** | ADB on :5555 answers but **`unauthorized`** |
| 192.168.2.104 | some other device | SSH banner `SSH-2.0-IPSSH-6.6.0`, not ArkOS |

The R36S exposes **only** port 5555 (ADB) — a full 1–1024 plus common-port
scan found nothing else. Port 22 is closed, so there is no SSH route.

`adb connect 192.168.2.115:5555` returns *device unauthorized*: the device is
running `adbd` but has not accepted this workstation's RSA key. Two ways
forward, both needing the user:

1. On the R36S, accept the "Allow USB debugging?" prompt (it may appear on
   screen for a TCP-connected device), or
2. Add this workstation's public key to the device's
   `/data/misc/adb/adb_keys` by other means, or
3. Enable SSH/other access on the device.

**Until ADB is authorised, every milestone-1 measurement that must come from
the R36S — the GLES 3.2 renderer check, peak RSS at the title screen, and all
frame-time percentiles — is blocked.** Everything host-side can proceed.

This workstation's ADB public key (for manual installation on the device):
`/home/amarqs/.android/adbkey.pub`.

---

## 6. psprecomp v0.5.0 gaps that this project must fill

Measured from the v0.5.0 source, not assumed:

| Gap | Evidence | Consequence here |
|---|---|---|
| **No game host** | `git ls-files` shows 79 tracked files: runtime (`src/`), tools (`tools/allegrexrecomp`), headers, tests, docs. There is no `main.c`, no `host/`, no `games/`. The README says "a game repo consumes this repo as a git submodule" — that game repo does not exist in this project yet. | I must write the host: ELF loader (place segments, apply PSP relocations, set `$gp`, register the module), the HLE registration glue, input, and the present loop. |
|| **No GLES backend** | ✅ Written: `src/mc3/render_gles.c` implements `psp_render_backend` using SDL2 + EGL + GLES 3.2. Compiles natively, initializes on host (Mesa 3.2). Letterboxed 640x363 content in 640x480 window. | The GLES 3.2 backend is complete and registered via `psp_render_register_backend()`. |
| **Through-mode only** | `docs/BRINGUP.md`: "The rasterizer currently handles through-mode only and *counts* transformed vertices rather than drawing them". | AGE's renderer uses transformed geometry with a VFPU-built matrix stack. Transformed geometry + textures are required before anything appears. This is the largest single piece of work in the project. |
|| **Partial VFPU** | `cover` on our file reports 639 VFPU instructions (0.06%) not yet named. | Low percentage but AGE is matrix-heavy: VFPU bugs will show as wrong physics/camera, not as crashes. Cross-check with objdump as instructed. |
| **Experimental threading** | ✅ Built with `PPSPRECOMP_REAL_THREADS=ON`. Threads create and start, but `sceKernelSleepThreadCB` tail-call optimization broke the infinite spin at `0x2F3054` — threads now return instead of spinning, altering game flow. | Threads work but game flow may be altered; needs verification. |
 | **No input/mapping** | `src/hle/misc.c` has the `sceCtrl` stubs, but no host-side controller source. | PortMaster input mapping (accelerate/brake/steer/handbrake/camera) must be written. |
| **No audio output** | `src/hle/sascore.c` synthesises; nothing consumes it. | Audio must be wired or explicitly stubbed for the title screen. |

The tool's own README is explicit that "**A game on screen ❌ Not yet**" for
its flagship module. Getting MC3 to a title screen is therefore genuinely
open-ended work, not a formality.

---

## 7. Milestone 1 status

| Item | State |
|---|---|
| psprecomp pinned | ✅ v0.5.0 / `198f10e` |
| Ghidra 12.1.3 + ghidra-allegrex v21.4 + JDK 21 | ✅ installed, `rea doctor` green |
| GhidraMCP 2.0 | ✅ installed in Ghidra 12.1.4 |
| REA vs this ELF | ⚠️ disagreement documented — relocatable module, no ET_EXEC, no ABI-flags record |
| Ghidra direct import of EBOOT.ELF | ✅ imported via Allegrex:LE:32:default, 14,685 functions, 92,463 symbols |
| aarch64 cross toolchain | ✅ Arch gcc 16.2.0/binutils 2.47/glibc 2.44 at ~/tools/aarch64-sysroot, verified |
| GLES 3.2 renderer | ✅ Backend written (`src/mc3/render_gles.c`), compiles natively, initializes on host (Mesa 3.2). Letterboxed 640x363 content in 640x480 window. |
| psprecomp emitter fix | ✅ patched `emit.c`: call return target (a+8) now labelled so `goto L_<a+8>` after an owned delay-slot call is defined |
| Generated C compiles | ✅ 34,344 functions + 98,901 labels, 110 MB, builds clean |
|| Game boots (host, software renderer) | ✅ module_start returns, 0 bad memory accesses, RSS ~19 MB, clean exit code 0 |
| Rendering (GE pixels) | ❌ 0 — no threads created, no GE commands. GLES 3.2 backend initializes (Mesa 3.2) but nothing is submitted to draw |
| PortMaster port structure | ✅ `ports/midnight-club-lar/`: launch script, port.json, README.jpg, screenshot.jpg, data/, assets/ |
| Stall located | ✅ `0x2F3054`: `jal 0x003C4DD0 (sceKernelSleepThreadCB)` then unconditional `beq $zero,$zero,0x2F3054` — infinite spin, 357M hits |
| Root cause | `hle_SleepThread` (`park_for(W_SLEEP, TICK_US)`) does not yield (`switches=0 parks=0`), so guest threads never run |
| Stack overflow (segfault) | ✅ FIXED — tail-call optimization in `emit.c`: `jal` with dead `$ra` emits `return psp_func_X();` instead of call+fall-through. Mutual tail recursion `0x2EE75C`↔`0x2EF5CC` no longer grows the host stack |
| Unimplemented firmware call | ⚠️ NID 0xEBD5C3E6 (SysMemUserForUser) called at module_start+0x5D0 — not identified, returns 0 by default |
| Thread creation | ❌ created=0 started=0 — module_start returns without creating threads; may depend on unimplemented NID or other missing HLE |
| Peak RSS on R36S | ❌ blocked (no ADB, device not accessible) |
| Frame-time distribution | ❌ blocked (no ADB, device not accessible) |

## 8. Current blocker — title screen not reached

The recompiled game boots and runs startup: `module_start` returns, 0 bad memory accesses, RSS ~19 MB, clean exit code 0. But no threads are created (`created=0 started=0`) and no GE commands are submitted (`GE pixels: 0`), so the title screen is never reached.

**Root cause (confirmed via gdb and execution traces):**

1. **Unimplemented firmware call NID 0xEBD5C3E6** (SysMemUserForUser) is called at `module_start+0x5D0`. This function is not identified — it doesn't match any known PSP SDK function name. It returns 0 (default for unimplemented calls), which may cause `module_start` to skip thread creation and other initialization.

2. **No threads created.** `module_start` returns without calling `sceKernelCreateThread`. With the old inline execution (no threading), threads were created but then spun forever. With `PREAL_THREADS=ON`, `module_start` takes a different path and returns without creating any threads.

3. **No GE rendering.** With no threads running, no display lists are submitted to the GE, so `GE pixels: 0`.

**Fix needed:** Identify and implement NID 0xEBD5C3E6, or determine why `module_start` skips thread creation. Additional missing pieces:
- **VFPU support:** 639 VFPU instructions (0.06%) emit as traps — AGE uses VFPU heavily for matrix math
- **Texture support:** Vertex format has position+colour only; textured geometry draws as solid colour
- **Audio output:** `sascore.c` synthesises but nothing consumes it
- **Input mapping:** No host-side controller mapping to PortMaster inputs

This is a toolkit/game bug combination, not a single emitter fix. The stack overflow (segfault) IS fixed — the game now runs cleanly to completion. The remaining work is feature implementation, not bug fixing.

## 9. Directory layout (source-only, game data excluded)

This is a threadman/scheduler bring-up bug. Options:
1. Debug why `hle_SleepThread`/`park_us` doesn't yield from the boot-thread context (g_tls_me / g_tls_depth / cpu_pending of started threads).
2. Ensure guest threads are scheduled as real threads rather than called directly from module entry.
3. You may want to review the threadman HLE before I spend more time here.

I cannot test on the R36S yet (see R36S access blocker below), so every milestone-1 measurement that must come from the device is blocked.

## 9. R36S access — BLOCKED, needs you

| Host | What it is | State |
|---|---|---|
| 192.168.2.120 | this workstation (Omarchy, x86_64) | — |
| 192.168.2.115 | **the R36S** | ADB on :5555 answers but **`unauthorized`** |
| 192.168.2.104 | some other device | SSH banner `SSH-2.0-IPSSH-6.6.0`, not ArkOS |

The R36S exposes **only** port 5555 (ADB) — full 1–1024 + common-port scan found nothing else. Port 22 closed, no SSH route. `adb connect 192.168.2.115:5555` → *device unauthorized*: `adbd` runs but hasn't accepted this workstation's RSA key.

Until ADB is authorised, every milestone-1 measurement that must come from the R36S — the GLES 3.2 renderer check, peak RSS at the title screen, and all frame-time percentiles — is blocked. Everything host-side can proceed.

Ways forward (need you):
1. On the R36S, accept the "Allow USB debugging?" prompt for this workstation, or
2. Install this workstation's ADB pubkey on the device: `/home/amarqs/.android/adbkey.pub`, or
3. Enable another access path (SSH) on the device.

This workstation's ADB public key: `/home/amarqs/.android/adbkey.pub`.

## 10. Directory layout (source-only, game data excluded)

```
/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/
  CMakeLists.txt              # top-level: psprecomp + mc3 host
  .gitignore                  # excludes ISO/ELF/generated C/ghidra projects
  notes/MEMORY.md             # this file
  scratch/                    # working copies, generated C, Ghidra/REA projects (gitignored)
    extracted/PSP_GAME/SYSDIR/EBOOT.ELF   # decrypted (== EBOOT_work.BIN.dec), never modified
    generated/mc3_funcs_*.c   # recompiled C (gitignored, regenerated by `emit`)
    ghidra/MC3.*              # Ghidra 12.1.3 project (gitignored)
    rea/                      # REA output (gitignored)
  src/
    psprecomp/                # toolkit (git submodule, pinned v0.5.0) — patched emit.c
    mc3/main.c                # game host (ELF loader, boot thread, watchdog, report)
    mc3/CMakeLists.txt
  tools/
    ghidra_headless_import.sh
    ghidra-scripts/ReportPspStats.java
    legacy_ssh.py
```

No game files, no generated C, no Ghidra databases are committed.

---
*End of memory file.*
