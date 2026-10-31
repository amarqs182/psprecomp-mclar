#!/usr/bin/env bash
# Import the decrypted EBOOT.ELF into Ghidra 12.1.3 + ghidra-allegrex v21.4
# using the Allegrex processor, headless. Writes a project under scratch/.
set -u
GHIDRA=${GHIDRA:-/home/amarqs/tools/ghidra_12.1.3_PUBLIC}
export JAVA_HOME=${JAVA_HOME:-/home/amarqs/.local/share/mise/installs/java/temurin-21.0.9+10.0.LTS}
export PATH="$JAVA_HOME/bin:$PATH"

PROJ_DIR="/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/scratch/ghidra"
TARGET="/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/scratch/extracted/PSP_GAME/SYSDIR/EBOOT.ELF"
mkdir -p "$PROJ_DIR"

"$GHIDRA/support/analyzeHeadless" \
  "$PROJ_DIR" MC3 \
  -import "$TARGET" \
  -processor "Allegrex:LE:32:default" \
  -overwrite \
  -analysisTimeoutPerFile 1800 \
  -log "$PROJ_DIR/import.log" \
  -scriptPath "/home/amarqs/recomp projects/Midnight Club - L.A. Remix (USA)/tools/ghidra-scripts" \
  -postScript ReportPspStats.java \
  2>&1 | tail -60
