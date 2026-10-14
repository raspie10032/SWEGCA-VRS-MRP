#!/usr/bin/env python3
"""N5: cause real OS notification loss, then verify conversion without restore.

Python 3.11 standard library only. All processes and files belong to --work.
An observed overflow is mandatory; an event burst alone cannot pass this test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import threading
import time


def line(file_id, number):
    return (json.dumps({"type": "user", "uuid": f"kwov-{file_id}-{number}",
                       "timestamp": f"2026-10-13T12:{number // 60:02}:{number % 60:02}.000Z",
                       "message": {"role": "user", "content": f"kwov evidence {file_id} change {number}"}},
                      ensure_ascii=False, separators=(",", ":")) + "\n").encode()


def cover(path):
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return -1
    if len(data) != 512:
        return -1
    return max(struct.unpack("<64Q", data))


class HandleSamples:
    """Test observer only: count OS resources on our own child throughout its run."""
    def __init__(self, proc):
        self.proc, self.samples, self.errors = proc, [], []
        self.stop = threading.Event()
        self.handle = None
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            self.ctypes, self.wintypes = ctypes, wintypes
            self.api = ctypes.WinDLL("kernel32", use_last_error=True)
            self.api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            self.api.OpenProcess.restype = wintypes.HANDLE
            self.api.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
            self.api.GetProcessHandleCount.restype = wintypes.BOOL
            self.api.CloseHandle.argtypes = [wintypes.HANDLE]
            self.handle = self.api.OpenProcess(0x1000, False, proc.pid)
            if not self.handle:
                raise OSError(ctypes.get_last_error(), "OpenProcess child")
        self.thread = threading.Thread(target=self.observe, daemon=True)
        self.thread.start()

    def observe(self):
        while not self.stop.is_set():
            try:
                if os.name == "nt":
                    count = self.wintypes.DWORD()
                    if not self.api.GetProcessHandleCount(self.handle, self.ctypes.byref(count)):
                        raise OSError(self.ctypes.get_last_error(), "GetProcessHandleCount")
                    value = count.value
                else:
                    value = len(list(Path(f"/proc/{self.proc.pid}/fd").iterdir()))
                self.samples.append((time.monotonic_ns(), value))
            except Exception as exc:
                if self.proc.poll() is None:
                    self.errors.append(str(exc))
            self.stop.wait(0.005)

    def finish(self, work):
        self.stop.set()
        self.thread.join()
        if self.handle:
            self.api.CloseHandle(self.handle)
        (work / "handle_samples.json").write_text(json.dumps(self.samples) + "\n", encoding="utf-8")
        values = [count for _, count in self.samples]
        return {"method": "GetProcessHandleCount" if os.name == "nt" else "/proc/pid/fd",
                "sample_interval_ms": 5, "samples": len(values), "min": min(values, default=0),
                "max_observed": max(values, default=0), "fixed_bound": 512, "errors": self.errors,
                "pass": bool(values) and not self.errors and max(values) <= 512}


def run_case(daemon, work, dormant):
    work.mkdir(parents=True, exist_ok=True)
    projects, data = work / "projects", work / "data"
    if projects.exists() or data.exists():
        raise RuntimeError("use a fresh --work directory")
    projects.mkdir()
    files = []
    # Names larger than the 64-byte Windows notification buffer force a real
    # zero-byte OS completion even if this machine consumes events very fast.
    for i in range(32):
        p = projects / (f"{i:03}-" + "x" * 96 + ".jsonl")
        p.write_bytes(line(i, 0))
        files.append(p)
    quiet = projects / "dormant"
    quiet.mkdir()
    dormant_paths = [quiet / f"quiet-{i:05}.jsonl" for i in range(dormant)]
    for path in dormant_paths:
        path.write_bytes(b'{"type":"progress"}\n')
    # Preserve every original oracle; W covers only active writers, not history.
    writers = work / "writers"
    from g3_writer_test import publish
    for p in files + dormant_paths[-1:]:
        publish(daemon.resolve(), writers, p, os.getpid())
    endpoint = ("\\\\.\\pipe\\vrs-kwov-" if os.name == "nt" else "@vrs-kwov-") + str(os.getpid())
    logpath = work / "daemon.log"
    env = dict(os.environ, VRS_CC_TEST_METRICS="1", VRS_CC_TEST_WATCH_BYTES="64")
    proc = None
    stopped = False
    result = {"dormant_files": dormant, "initial_files": 32 + dormant}
    with logpath.open("wb") as log:
        proc = subprocess.Popen([str(daemon.resolve()), "--projects", str(projects),
                                 "--data", str(data), "--endpoint", endpoint, "--writer-markers", str(writers),
                                 "--workers", "6", "--connections", "4"],
                                stdout=subprocess.DEVNULL, stderr=log, env=env)
        monitor = HandleSamples(proc)
        def logs():
            return logpath.read_text(encoding="utf-8", errors="replace")

        def wait_for(predicate, label, timeout=120):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                if proc.poll() is not None:
                    raise AssertionError(f"daemon exited {proc.returncode}: {logs()[-2000:]}")
                if predicate():
                    return
                time.sleep(0.025)
            raise AssertionError(f"timeout: {label}: {logs()[-2000:]}")

        def covered():
            return all(cover(data / "cover" / (str(p.relative_to(projects)) + ".cv")) == p.stat().st_size for p in set(files + dormant_paths))

        def validate():
            output_lines = 0
            for p in files:
                original = p.read_bytes()
                expected, offset = {}, 0
                for raw in original.splitlines(keepends=True):
                    row = json.loads(raw)
                    if row["type"] == "user":
                        expected[offset] = (raw[:-1], row["message"]["content"])
                    offset += len(raw)
                actual = set()
                path = data / "cc_rollout" / p.relative_to(projects)
                for raw in path.read_bytes().splitlines():
                    row = json.loads(re.sub(rb'("ordinal":)0*(\d+)', rb'\1\2', raw, count=1))
                    payload = row["payload"]
                    if "cc" not in payload:
                        continue
                    c = payload["cc"]
                    source, text = expected[c["o"]]
                    assert len(source) == c["n"] and hashlib.sha256(source).hexdigest() == c["h"], (p, c)
                    if payload.get("type") == "message":
                        assert payload["role"] == "user" and payload["content"][0]["text"] == text
                        assert c["o"] not in actual, (p, "duplicate", c)
                        actual.add(c["o"])
                        output_lines += 1
                assert actual == set(expected), (p, "missing", set(expected) - actual)
            return output_lines

        try:
            wait_for(lambda: "VRS_TEST_WATCH_READY" in logs() and "VRS_TEST_OPEN" in logs() and covered(), "initial conversion")
            if os.name != "nt":
                # Stop ONLY the child created above. The kernel still collects
                # notifications while this process cannot consume any of them.
                os.kill(proc.pid, signal.SIGSTOP)
                stopped = True
                capacity = int(Path("/proc/sys/fs/inotify/max_queued_events").read_text())
                assert capacity <= 1048576, "inotify capacity too large for this bounded fixture"
                for i in range(capacity + 64):
                    (projects / f"noise-{i}.tmp").touch()
                result["inotify_capacity"] = capacity
            else:
                for i in range(128):
                    (projects / ("noise-" + "y" * 96 + f"-{i}.tmp")).touch()
            for i, p in enumerate(files):
                with p.open("ab") as stream:
                    stream.write(b"".join(line(i, k) for k in range(1, 5)))
            if dormant_paths:
                with dormant_paths[-1].open("ab") as stream:
                    stream.write(line(500, 4))
                files.append(dormant_paths[-1])
            nested = projects / "new-directory" / "nested"
            nested.mkdir(parents=True)
            for i in range(4):
                p = nested / ("new-" + "z" * 96 + f"-{i}.jsonl")
                p.write_bytes(line(100 + i, 4))
                files.append(p)
                publish(daemon.resolve(), writers, p, os.getpid())
            if stopped:
                os.kill(proc.pid, signal.SIGCONT)
                stopped = False
            wait_for(lambda: "VRS_TEST_OVERFLOW" in logs() and "VRS_TEST_RESCAN" in logs() and covered(), "overflow recovery")
            result["recovered_message_lines"] = validate()
            # A later change to an old and a newly discovered file proves that
            # recovery did not drop the live subscription.
            for i, p in enumerate((files[0], files[-1])):
                with p.open("ab") as stream:
                    stream.write(line(200 + i, 5))
            wait_for(covered, "post-recovery changes")
            result["final_message_lines"] = validate()
            for path in dormant_paths[:-1]:
                assert path.read_bytes() == b'{"type":"progress"}\n'
                assert not (data / "cc_rollout" / path.relative_to(projects)).exists()
            result["final_transcripts"] = 36 + dormant
            text = logs()
            assert "conversion:" not in text and "watch:" not in text, text[-3000:]
            result.update({"pass": True, "files": len(files), "covers_equal_eof": True,
                           "overflow_count": len(re.findall(r"^VRS_TEST_OVERFLOW", text, re.M)),
                           "rescan_count": len(re.findall(r"^VRS_TEST_RESCAN", text, re.M)),
                           "restore_calls": 0, "metrics": [s for s in text.splitlines() if s.startswith(("VRS_TEST_START ", "VRS_TEST_SCAN ", "VRS_TEST_PRELIB ", "VRS_TEST_OPEN "))]})
            # Read only this relay request's optional benchmark logs. The relay
            # returns our stdout but does not otherwise return daemon metrics.
            stages = {}
            for path in sorted(work.parent.glob("*.err")):
                rows = [s for s in path.read_text(encoding="utf-8", errors="replace").splitlines()
                        if s.startswith(("VRS_TEST_START ", "VRS_TEST_SCAN ", "VRS_TEST_PRELIB ", "VRS_TEST_OPEN ", "VRS_TEST_FIRST_RESTORE ", "VRS_TEST_RESTORE "))]
                if rows:
                    stages[path.name] = rows[-12:]
            if stages:
                result["relay_stage_metrics"] = stages
        finally:
            result["handles"] = monitor.finish(work)
            if stopped:
                os.kill(proc.pid, signal.SIGCONT)
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
    (work / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    assert result["handles"]["pass"], result["handles"]
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon", required=True, type=Path)
    ap.add_argument("--work", required=True, type=Path)
    args = ap.parse_args()
    work = args.work.resolve()
    limits = None
    if os.name != "nt":
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if soft > 4096:
            resource.setrlimit(resource.RLIMIT_NOFILE, (4096, hard))
        limits = resource.getrlimit(resource.RLIMIT_NOFILE)
        assert limits[0] == 4096, ("requires soft NOFILE 4096, never raises it", limits)
    # Preserve the original 36-file fixture, then G's two unchanged scale conditions.
    cases = [run_case(args.daemon, work / f"n{n}", n) for n in (0, 32, 4096)]
    keys = ("initial_files", "final_transcripts", "files", "covers_equal_eof",
            "recovered_message_lines", "final_message_lines", "overflow_count",
            "rescan_count", "restore_calls", "handles")
    result = {"pass": all(case["pass"] and case["handles"]["pass"] for case in cases),
              "nofile": limits, "cases": [{k: case[k] for k in keys} for case in cases],
              "overflow_cost": "G3 M/W witnesses; writer lifecycle premise"}
    (work / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    from g2_marker_test import run_suite
    result["g2_markers"] = run_suite(args.daemon, work / "g2-markers")
    from g3_writer_test import run_suite as writer_suite
    result["g3_writers"] = writer_suite(args.daemon, work / "g3-writers")
    result["pass"] = result["pass"] and result["g2_markers"]["pass"] and result["g3_writers"]["pass_"]
    w5_trials = work.parent / "w5root" / "work" / "trials.jsonl"
    if w5_trials.exists():
        trials = [json.loads(line) for line in w5_trials.read_text(encoding="utf-8").splitlines() if line]
        bad = [r for r in trials if not (r["ready_before_kill"] and r["alive_before_kill"]
               and r["kill_rc"] == (1 if os.name == "nt" else -9)
               and r["reopen"]["rc"] == 0 and r["reopen"]["ready"])]
        result["w5_diagnostics"] = {"trials": len(trials), "bad": bad}
        for row in bad:
            path = w5_trials.parent / f'kill-{row["i"]:03}.log'
            row["kill_log"] = path.read_text(encoding="utf-8", errors="replace") if path.exists() else "missing"
    (work / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # The fixed relay returns only the last 3000 characters. Keep full evidence
    # on disk and return every acceptance condition in one bounded summary.
    def handles(row):
        h = row["handles"]
        return {"max": h["max_observed"], "bound": h["fixed_bound"], "pass": h["pass"], "errors": len(h["errors"])}
    brief = {"pass": result["pass"], "nofile": limits, "n5": [
        {"N": r["final_transcripts"], "overflow": r["overflow_count"], "rescan": r["rescan_count"],
         "messages": [r["recovered_message_lines"], r["final_message_lines"]], "cover": r["covers_equal_eof"],
         "restore": r["restore_calls"], "handles": handles(r), "pass": r["pass"]} for r in cases],
        "g2": [{"N": r["initial_files"], "delta": r["changed_files"],
                "visit_max": max(n for n, _ in r["marker_scans"]), "messages": r["final_messages"],
                "overflow": r["overflow_count"], "remaining": r["remaining_markers"],
                "handles": handles(r), "pass": r["pass"]} for r in result["g2_markers"]["cases"]],
        "g3": [{"N": r["comparison_files"], "delta": r["changed_files"], "main": r["main"],
                "messages": r["messages"], "restore": r["restore_calls"], "handles": handles(r),
                "lib_callback": any(x["origin"] == "lib" for x in r["scans"]), "pass": r["pass"]}
               for r in result["g3_writers"]["cases"]],
        "boundaries": result["g3_writers"]["cases"][0]["boundary_cases"],
        "extra": result["g3_writers"]["cases"][0]["extra_cases"],
        "hooks": [{"host": r["host"], "pass": r["pass_"], "us": r["invocations_us"], "invalid_rc": r["invalid_hook_rc"]}
                  for r in result["g3_writers"]["hooks"]],
        "retirement": result["g3_writers"]["periodic_retirement"]}
    if "w5_diagnostics" in result:
        w5 = result["w5_diagnostics"]
        brief["w5_trials"] = w5["trials"]
        brief["w5_bad"] = len(w5["bad"])
    compact = json.dumps(brief, ensure_ascii=False, separators=(",", ":"))
    assert len(compact) < 3000, "relay evidence would be truncated"
    print(compact)
    if result.get("w5_diagnostics", {}).get("bad"):
        print("W5_BAD " + json.dumps([{k: r[k] for k in ("i", "ready_before_kill", "alive_before_kill", "kill_rc", "insert_receipts")}
                                     | {"log_tail": r["kill_log"][-350:]} for r in result["w5_diagnostics"]["bad"][:2]], separators=(",", ":")))
    # Fixed prior acceptance evidence, read-only: explain g3-003's hidden W5
    # failure instead of discarding it after a later passing run.
    prior_trials = work.parent.parent / "g3-003" / "w5root" / "work" / "trials.jsonl"
    if prior_trials.exists():
        prior = [json.loads(line) for line in prior_trials.read_text(encoding="utf-8").splitlines() if line]
        failures = [r for r in prior if not (r["ready_before_kill"] and r["alive_before_kill"]
                    and r["kill_rc"] == (1 if os.name == "nt" else -9)
                    and r["reopen"]["rc"] == 0 and r["reopen"]["ready"])]
        for row in failures:
            path = prior_trials.parent / f'kill-{row["i"]:03}.log'
            row["kill_log"] = path.read_text(encoding="utf-8", errors="replace") if path.exists() else "missing"
        (work / "g3-003-w5-evidence.json").write_text(json.dumps({"trials": len(prior), "failures": failures}, indent=2) + "\n", encoding="utf-8")
        print("PRIOR_G3_003_W5 " + json.dumps({"trials": len(prior), "failed": len(failures), "failures": [
            {k: r[k] for k in ("i", "ready_before_kill", "alive_before_kill", "kill_rc", "insert_receipts")}
            | {"log_tail": r["kill_log"][-350:]} for r in failures[:2]]}, separators=(",", ":")))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
