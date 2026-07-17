#!/usr/bin/env python3
"""
Local save data inventory script for macOS.
Scans common emulator save locations and reports structural facts only.
NEVER prints game titles, save contents, or identifying information.

This script is NOT part of `make test` and should not be committed
with any output it produces.
"""
import os
import sys
from pathlib import Path

HOME = Path.home()

# macOS emulator save locations (verified against Freegosy source + known conventions)
LOCATIONS = {
    "Dolphin (GameCube)": [
        HOME / "Library/Application Support/Dolphin/GC",
    ],
    "Dolphin (Wii)": [
        HOME / "Library/Application Support/Dolphin/Wii",
    ],
    "PPSSPP": [
        HOME / "Library/Application Support/PPSSPP/PSP/SAVEDATA",
        HOME / "Documents/PPSSPP/PSP/SAVEDATA",
    ],
    "PCSX2": [
        HOME / "Library/Application Support/PCSX2/memcards",
        HOME / "Library/Application Support/PCSX2/saves",
    ],
    "RetroArch": [
        HOME / "Library/Application Support/RetroArch/saves",
    ],
    "DuckStation": [
        HOME / "Library/Application Support/DuckStation/memcards",
    ],
    "Ryujinx": [
        HOME / "Library/Application Support/Ryujinx/bis/user/save",
    ],
    "RPCS3": [
        HOME / "Library/Application Support/rpcs3/dev_hdd0/home/00000001/savedata",
    ],
    "Cemu": [
        HOME / "Library/Application Support/Cemu/mlc01/usr/save",
    ],
    "Freegosy": [
        HOME / "Library/Application Support/Freegosy",
        HOME / ".local/share/Freegosy",
    ],
    "mGBA": [
        HOME / "Library/Application Support/mGBA",
    ],
    "melonDS": [
        HOME / "Library/Application Support/melonDS",
    ],
}

def scan_location(path):
    """Scan a directory and return structural facts only."""
    if not path.exists():
        return {"exists": False}

    result = {"exists": True, "file_count": 0, "dir_count": 0,
              "extensions": set(), "depth": 0, "total_size": 0}

    try:
        for item in path.rglob("*"):
            if item.is_file():
                result["file_count"] += 1
                ext = item.suffix.lower()
                if ext:
                    result["extensions"].add(ext)
                try:
                    result["total_size"] += item.stat().st_size
                except (OSError, PermissionError):
                    pass
            elif item.is_dir():
                result["dir_count"] += 1

        # Calculate max depth
        rel_parts = [p for p in path.rglob("*") if p.is_dir()]
        if rel_parts:
            result["depth"] = max(len(p.relative_to(path).parts) for p in rel_parts)
    except (PermissionError, OSError):
        pass

    result["extensions"] = sorted(result["extensions"])
    return result

def main():
    print(f"{'Platform':<25} {'Path':<55} {'Exists':<7} {'Files':<7} {'Dirs':<6} {'Extensions':<25} {'Size':<12}")
    print("-" * 150)

    for platform, paths in LOCATIONS.items():
        for path in paths:
            info = scan_location(path)
            exists = "Yes" if info["exists"] else "No"
            files = str(info.get("file_count", ""))
            dirs = str(info.get("dir_count", ""))
            exts = ", ".join(info.get("extensions", [])[:5])
            if len(info.get("extensions", [])) > 5:
                exts += ", ..."
            size = info.get("total_size", 0)
            if size > 1024*1024:
                size_str = f"{size/1024/1024:.1f} MB"
            elif size > 1024:
                size_str = f"{size/1024:.1f} KB"
            else:
                size_str = f"{size} B" if size else ""

            print(f"{platform:<25} {str(path):<55} {exists:<7} {files:<7} {dirs:<6} {exts:<25} {size_str:<12}")

if __name__ == "__main__":
    main()
