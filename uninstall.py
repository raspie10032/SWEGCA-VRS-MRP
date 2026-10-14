"""vrs-lib 1.0.0 uninstaller: removes the vrs-lib hooks and MCP entry. The data directory is left in place.

python uninstall.py [--settings PATH] [--claude PATH] [--skip-mcp]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from install import HOME, find_claude, load, save, strip_owned


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--settings", default=str(HOME / ".claude" / "settings.json"))
    ap.add_argument("--claude", default=None)
    ap.add_argument("--skip-mcp", action="store_true")
    a = ap.parse_args()
    settings_path = Path(a.settings)
    settings = load(settings_path)
    removed = strip_owned(settings)
    backup = save(settings_path, settings) if removed else None
    print(f"settings: removed {removed} vrs-lib hook(s){f'; backup {backup}' if backup else ''}")
    if a.skip_mcp:
        return 0
    claude = a.claude or find_claude()
    if not claude:
        print("note: claude CLI not found; run: claude mcp remove vrs-lib -s user")
        return 0
    r = subprocess.run([claude, "mcp", "remove", "vrs-lib", "-s", "user"], capture_output=True, text=True)
    print("mcp: vrs-lib removed" if r.returncode == 0 else f"mcp: {r.stderr.strip() or r.stdout.strip()}")
    print("The data directory was not deleted. A running daemon stays up (it is detached); to stop it:")
    print("  Get-Process vrs_ccd | Stop-Process")
    return 0


if __name__ == "__main__":
    sys.exit(main())
