#!/usr/bin/env python3
"""Compare the N-A Linux build with the fixed lib7 batch/reopen outputs.

Run under taskset -c 0-11. Output is confined to a new --work directory;
reference files and source sessions are read only. Only the terminal us field
is removed from assembly JSON. The complete ordered rc list is checked.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--ref", required=True, type=Path)
    ap.add_argument("--m1", required=True, type=Path)
    ap.add_argument("--kx2", required=True, type=Path)
    ap.add_argument("--work", required=True, type=Path)
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=False)
    result = {"binary_sha256": hashlib.sha256(a.binary.read_bytes()).hexdigest(), "tests": {}}
    strip_us = re.compile(rb',"us":\d+\}$')
    for name, sessions in (("m1", a.m1), ("kx2", a.kx2)):
        for mode in ("batch", "reopen"):
            dst = a.work / name / mode
            dst.mkdir(parents=True)
            ref = a.ref / name
            cmd = [str(a.binary.resolve()), "--stats", "--workers", "6", "--out", str(dst),
                   str(sessions), str(a.work / name / "index"), "@" + str(ref / "list.txt")]
            run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            (a.work / name / (mode + ".log")).write_text(run.stdout + run.stderr, encoding="utf-8")
            ref_log = (ref / (mode + ".log")).read_text(encoding="utf-8")
            rc = re.findall(r"^assemble rc=(-?\d+) ", run.stdout, re.M)
            ref_rc = re.findall(r"^assemble rc=(-?\d+) ", ref_log, re.M)
            my_hash = re.findall(r"cells_hash=([0-9a-f]+)", run.stdout)
            ref_hash = re.findall(r"cells_hash=([0-9a-f]+)", ref_log)
            want = sorted(p.name for p in (ref / mode).iterdir())
            actual = sorted(p.name for p in dst.iterdir())
            diff = [n for n in want if not (dst / n).exists() or
                    strip_us.sub(b"}", (dst / n).read_bytes()) !=
                    strip_us.sub(b"}", (ref / mode / n).read_bytes())]
            entry = {"process_rc": run.returncode, "cells_hash": my_hash, "ref_cells_hash": ref_hash,
                     "assemble_count": len(rc), "ref_assemble_count": len(ref_rc),
                     "rc_list_equal": rc == ref_rc, "json_count": len(actual), "json_diff": diff,
                     "json_names_equal": actual == want,
                     "ready_ms": re.findall(r"ready_ms=([\d.]+)", run.stdout),
                     "shards_rebuilt": re.findall(r"shards_rebuilt=(\d+)", run.stdout)}
            entry["pass"] = (run.returncode == (1 if any(r != "0" for r in ref_rc) else 0)
                             and bool(my_hash) and my_hash == ref_hash and rc == ref_rc
                             and len(rc) == len(want) and actual == want and not diff)
            result["tests"][name + "_" + mode] = entry
            print(name, mode, json.dumps(entry), flush=True)
            (a.work / "summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    result["pass"] = all(t["pass"] for t in result["tests"].values())
    (a.work / "summary.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
