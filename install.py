"""vrs-lib 1.0.0 installer for Claude Code on Windows.

Registers the vrs-lib MCP server (user scope) and the three hooks it needs in ~/.claude/settings.json.
Run from the cloned folder with the CPython that should run the bridge:  python install.py [--data DIR] [--claude PATH]
The clone is the install: hooks and the MCP entry point at this folder, so keep it where it is (move it = install again).
Re-running replaces earlier vrs-lib entries instead of adding duplicates. settings.json is backed up first.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
EXE = ROOT / "bin" / "vrs_ccd.exe"
BRIDGE = ROOT / "bridge" / "vrs_lib_bridge.py"
HOOK = ROOT / "hooks" / "hook_compact.py"
HOME = Path.home()
# Entries this package owns: the writer-marker hook and the compaction guidance hook of any vrs-lib install.
OWNED = re.compile(r'vrs_ccd\.exe"?\s+--writer-hook|vrs-lib[^"]*hook_compact\.py')


def fwd(p: Path | str) -> str:
    return str(p).replace("\\", "/")


def find_claude() -> str | None:
    found = shutil.which("claude")
    if found:
        return found
    pattern = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Packages", "Claude_*", "LocalCache", "Roaming",
                           "Claude", "claude-code", "*", "*", "claude.exe")
    hits = sorted(glob.glob(pattern), key=os.path.getmtime)
    return hits[-1] if hits else None


def strip_owned(settings: dict) -> int:
    removed = 0
    hooks = settings.get("hooks")
    if not isinstance(hooks, dict):
        return 0
    for event in list(hooks):
        groups = hooks[event]
        if not isinstance(groups, list):
            continue
        kept_groups = []
        for group in groups:
            inner = group.get("hooks", []) if isinstance(group, dict) else []
            kept = [h for h in inner if not (isinstance(h, dict) and OWNED.search(str(h.get("command", ""))))]
            removed += len(inner) - len(kept)
            if kept or not inner:
                if kept != inner:
                    group = dict(group, hooks=kept)
                kept_groups.append(group)
        if kept_groups:
            hooks[event] = kept_groups
        else:
            del hooks[event]
    if not hooks:
        del settings["hooks"]
    return removed


def load(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8") or "{}")


def save(path: Path, data: dict) -> Path | None:
    backup = None
    if path.exists():
        # Never replace an earlier backup: two runs in the same second must keep both.
        stamp, n = time.strftime('%Y%m%d-%H%M%S'), 0
        while True:
            backup = path.with_name(f"{path.name}.bak-vrslib-{stamp}" + (f"-{n}" if n else ""))
            try:
                with open(backup, "xb") as f:
                    f.write(path.read_bytes())
                break
            except FileExistsError:
                n += 1
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return backup


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=str(HOME / ".vrs-lib" / "data"), help="daemon data directory (index, markers)")
    ap.add_argument("--projects", default=str(HOME / ".claude" / "projects"), help="Claude Code transcript root")
    ap.add_argument("--settings", default=str(HOME / ".claude" / "settings.json"), help="settings.json to edit")
    ap.add_argument("--claude", default=None, help="claude CLI used for `claude mcp add-json` (auto-detected)")
    ap.add_argument("--skip-mcp", action="store_true", help="edit settings.json only; print the MCP entry instead")
    a = ap.parse_args()

    if "WindowsApps" in sys.executable:
        print("error: this is the Microsoft Store python stub; run install.py with a real CPython.", file=sys.stderr)
        return 2
    for need in (EXE, BRIDGE, HOOK):
        if not need.is_file():
            print(f"error: missing {need}", file=sys.stderr)
            return 2
    # A clone that rewrote line endings or fetched an LFS pointer instead of the binary must not be installed.
    sums = ROOT / "SHA256SUMS.txt"
    if sums.is_file():
        want = {p.strip(): h for h, p in (line.split(None, 1) for line in sums.read_text(encoding="utf-8").splitlines() if line.strip())}
        for need in (EXE, BRIDGE, HOOK):
            rel = need.relative_to(ROOT).as_posix()
            if want.get(rel) != hashlib.sha256(need.read_bytes()).hexdigest():
                print(f"error: {rel} does not match SHA256SUMS.txt (damaged checkout?)", file=sys.stderr)
                return 2
    data = Path(a.data)
    (data / "writer-markers").mkdir(parents=True, exist_ok=True)

    writer = f'"{fwd(EXE)}" --writer-hook "{fwd(data / "writer-markers")}" --host claude'
    guide = f'"{fwd(sys.executable)}" "{fwd(HOOK)}"'
    settings_path = Path(a.settings)
    settings = load(settings_path)
    removed = strip_owned(settings)
    hooks = settings.setdefault("hooks", {})
    hooks.setdefault("SessionStart", []).append({"matcher": "", "hooks": [{"type": "command", "command": writer}]})
    hooks["SessionStart"].append({"matcher": "compact", "hooks": [{"type": "command", "command": guide}]})
    hooks.setdefault("UserPromptSubmit", []).append({"matcher": "", "hooks": [{"type": "command", "command": writer}]})
    backup = save(settings_path, settings)
    print(f"settings: {settings_path} (replaced {removed} earlier vrs-lib hook(s); backup {backup})")

    entry = {"type": "stdio", "command": sys.executable, "args": ["-u", str(BRIDGE)],
             "env": {"VRS_CC_DATA": str(data), "VRS_CC_PROJECTS": str(Path(a.projects)), "VRS_CC_DAEMON": str(EXE)}}
    payload = json.dumps(entry, ensure_ascii=False)
    claude = a.claude or find_claude()
    if a.skip_mcp or not claude:
        if not a.skip_mcp:
            print("note: claude CLI not found; register the MCP server yourself:")
        print(f"  claude mcp add-json vrs-lib '{payload}' -s user")
        return 0
    subprocess.run([claude, "mcp", "remove", "vrs-lib", "-s", "user"], capture_output=True)
    r = subprocess.run([claude, "mcp", "add-json", "vrs-lib", payload, "-s", "user"], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"error: claude mcp add-json failed: {r.stderr.strip() or r.stdout.strip()}", file=sys.stderr)
        return 1
    print(f"mcp: vrs-lib registered (user scope) via {claude}")
    print("done. New Claude Code sessions get vrs_restore / vrs_read; open sessions keep their old configuration.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
