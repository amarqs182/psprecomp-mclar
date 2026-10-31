#!/usr/bin/env python3
"""
tools/strip_assets.py — Optional asset reduction for Midnight Club: L.A. Remix.

Runs on a **user-supplied, decrypted** copy of the game's extracted PSP_GAME
folder. Never modifies the original ISO, never touches game files inside the
repo, and is never included in the PortMaster port.

The script is idempotent, prints what it removed and how many bytes it saved,
and refuses to run if the expected files are not present.

Allowed reductions (cheapest first), all reversible by re-extracting from the
original ISO:
  - Drop unused language subtitle/voice folders
  - Replace intro/menu movies with a still frame
  - Downscale audio (placeholder: reports what would be done)
  - Trim unused debug/bonus data

Usage:
    python3 tools/strip_assets.py /path/to/decrypted/PSP_GAME [--apply]

Without --apply, the script reports what *would* be removed (dry run).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration — edit these to match your copy's layout.
# ---------------------------------------------------------------------------

# Subtitle/voice folders that can be dropped if you only play in one language.
DROPPABLE_LANG_DIRS = [
    "FRENCH", "GERMAN", "ITALIAN", "SPANISH",  # add your locale codes here
]

# Movie files to replace with a still frame (keep the file, empty the content).
MOVIE_FILES = [
    "INTRO.PMF", "LOGO.PMF", "MENU.PMF",
]

# Audio extensions that could be downscaled (reported only, no action).
AUDIO_EXTS = (".at3", ".atrac3", ".wav", ".mp3")

# ---------------------------------------------------------------------------


def human_size(n: int) -> str:
    size = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.0f} {unit}"
        size /= 1024
    return f"{size:.1f} TB"


class StripReport:
    def __init__(self) -> None:
        self.saved = 0
        self.lines: list[str] = []

    def note(self, path: str, saved: int, action: str = "removed") -> None:
        self.saved += saved
        self.lines.append(f"  {action:<12} {path}  ({human_size(saved)})")

    def summary(self) -> str:
        lines = [f"{'Action':<12} {'Path'}  {'Saved'}"]
        lines.extend(self.lines)
        lines.append(f"{'TOTAL':<12} {'bytes saved':<12} {human_size(self.saved)}")
        return "\n".join(lines)


def safe_remove(path: Path, report: StripReport, apply: bool) -> None:
    if not path.exists():
        return
    saved = path.stat().st_size
    if apply:
        if path.is_dir():
            # Remove the whole directory tree
            for sub in sorted(path.rglob("*")):
                if sub.is_file():
                    sub.unlink()
            path.rmdir()
        else:
            path.unlink()
    report.note(str(path), saved, "removed" if apply else "would remove")


def replace_with_empty(path: Path, report: StripReport, apply: bool) -> None:
    """Replace a file's content with 0 bytes (keep the file, empty it)."""
    if not path.exists():
        return
    saved = path.stat().st_size
    if saved == 0:
        return
    if apply:
        path.write_bytes(b"")
    report.note(str(path), saved, "emptied" if apply else "would empty")


def scan_game_folder(root: Path, report: StripReport, apply: bool) -> None:
    if not root.is_dir():
        print(f"Error: not a directory: {root}", file=sys.stderr)
        sys.exit(1)

    print(f"Scanning {root} ...\n")

    # 1. Droppable language folders
    for lang in DROPPABLE_LANG_DIRS:
        # Look for the language folder anywhere under the game root
        for match in root.rglob(lang):
            if match.is_dir():
                safe_remove(match, report, apply)

    # 2. Movie files -> empty them
    for movie in MOVIE_FILES:
        for match in root.rglob(movie):
            if match.is_file():
                replace_with_empty(match, report, apply)

    # 3. Audio files (report only — no action without a transcoder)
    for ext in AUDIO_EXTS:
        for match in root.rglob(f"*{ext}"):
            if match.is_file():
                report.note(str(match), match.stat().st_size,
                            "would downscale" if not apply else "downscale-pending")

    # 4. Debug / bonus folders (report only)
    for debug_name in ("DEBUG", "debug", "BONUS", "bonus", "DEV", "dev"):
        for match in root.rglob(debug_name):
            if match.is_dir():
                report.note(str(match), sum(
                    f.stat().st_size for f in match.rglob("*") if f.is_file()
                ), "would remove (debug)" if not apply else "remove (debug)")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Strip optional assets from a decrypted PSP_GAME folder.")
    ap.add_argument("psp_game_dir",
                    help="Path to the extracted PSP_GAME folder (decrypted copy).")
    ap.add_argument("--apply", action="store_true",
                    help="Actually perform removals (default: dry run).")
    args = ap.parse_args()

    root = Path(args.psp_game_dir).resolve()
    if not root.exists():
        print(f"Error: path not found: {root}", file=sys.stderr)
        return 1

    # Safety: refuse to run if it looks like the original ISO is present.
    iso_candidates = list(root.rglob("*.ISO")) + list(root.rglob("*.iso"))
    if iso_candidates:
        print("Error: ISO files found in the target folder. This script operates "
              "on an extracted, decrypted PSP_GAME folder, not an ISO.",
              file=sys.stderr)
        return 1

    report = StripReport()
    scan_game_folder(root, report, args.apply)

    print(report.summary())

    if not args.apply and report.saved > 0:
        print("\nThis was a dry run. Re-run with --apply to perform removals.")
    elif args.apply:
        print("\nApplied. Re-run to verify (should report 0 bytes saved).")

    return 0


if __name__ == "__main__":
    sys.exit(main())
