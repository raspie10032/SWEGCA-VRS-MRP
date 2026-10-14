#!/usr/bin/env python3
"""Independent original-byte verification. Criteria fixed in PLAN.md."""
from __future__ import annotations
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "src/tests"
OUT = Path(os.environ.get("VRS_CC_TEST_OUTPUT", str(TESTS)))
OUT.mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix="u-", dir=OUT))
BIN = Path(os.environ.get("VRS_CC_TEST_BIN", str(ROOT / "src/build/u_native")))
DAEMON = Path(os.environ.get("VRS_CC_TEST_DAEMON", str(ROOT / "src/build/vrs_ccd")))
SAMPLES = Path(os.environ.get("VRS_CC_TEST_SAMPLES", str(ROOT / "sample/projects")))
results = []
details = {}


def report(name, ok, detail):
    line = f"{name} {'PASS' if ok else 'FAIL'} {detail}"
    print(line, flush=True)
    results.append(line)


def run(*args):
    return subprocess.check_output([str(a) for a in args], encoding="utf-8", stderr=subprocess.STDOUT)


def rollout_load(raw):
    at = raw.index(b',"ordinal":') + 11
    digits = raw[at:at + 10]
    assert re.fullmatch(rb"[0-9]{10}", digits)
    return json.loads(raw[:at] + str(int(digits)).encode() + raw[at + 10:])


def independent_body(j, k, name, cc):
    if j["type"] == "attachment":
        assert j["attachment"]["type"] == "queued_command" and j["attachment"]["origin"]["kind"] == "human"
        return j["attachment"]["prompt"]
    content = j["message"]["content"]
    if isinstance(content, str):
        assert k == 0
        return content
    b = content[k]
    if b["type"] == "image":
        return f"[image {b['source']['media_type']} · {name}#{cc['o']}+{cc['n']} k={cc['k']}]"
    assert b["type"] == "text"
    return b["text"]


def units(s):
    return len(s.encode("utf-16-le")) // 2


def ku1(text):
    # Reproduce the observed lib7 classification separately, count the narrower
    # document exception and actual implementation exceptions in metrics.
    text = text.lstrip(" \r\n\t")
    if not (text.startswith("<") or text.startswith("# AGENTS.md")):
        return False
    if text.startswith(("<image", "</image", "<send_user_message_question_reply")):
        return False
    while text:
        text = text.lstrip(" \r\n\t")
        if not text:
            return True
        if text.startswith("<"):
            tag = re.match(r"<([^ >/\n]+)", text)
            if not tag:
                return True
            close = "</" + tag[1] + ">"
            at = text.find(close)
            if at < 0:
                return True
            text = text[at + len(close):]
        elif text.startswith("# AGENTS.md"):
            at = text.find("</INSTRUCTIONS>")
            if at < 0:
                return True
            text = text[at + 15:]
        else:
            return False
    return True


def metrics(original, rollout):
    rows = []
    at = 0
    for raw in original.splitlines(keepends=True):
        if raw.endswith(b"\n"):
            rows.append((at, json.loads(raw)))
        at += len(raw)
    timestamps = [j["timestamp"] for _, j in rows if "timestamp" in j]
    m = {"KT1_clear_while_open": 0, "KT2": 0, "KU1_document": 0, "KU1_lib": 0, "KP1c_timestamped": sum(a > b for a, b in zip(timestamps, timestamps[1:]))}
    message_counts = {}
    opened = False
    previous_ts = None
    decrease = 0
    by_offset = dict(rows)
    spoken = 0
    for raw in rollout.splitlines():
        j = rollout_load(raw)
        p = j["payload"]
        if "cc" not in p:
            continue
        if previous_ts is not None and previous_ts > j["timestamp"]:
            decrease += 1
        previous_ts = j["timestamp"]
        if p["type"] == "task_started":
            opened = True
        if p["type"] == "task_complete":
            opened = False
        if p["type"] == "message" and p.get("role") == "user":
            spoken += 1
            text = p["content"][0].get("text", "")
            m["KU1_lib"] += ku1(text)
            # The document exception requires complete paired tag blocks.
            complete_tags = text.strip()
            while complete_tags.startswith("<") and not complete_tags.startswith(("<image", "</image")):
                tag = re.match(r"<([A-Za-z_][A-Za-z0-9_:-]*)(?:\s[^>]*)?>", complete_tags)
                if not tag:
                    break
                close = "</" + tag[1] + ">"
                end = complete_tags.find(close, tag.end())
                if end < 0:
                    break
                complete_tags = complete_tags[end + len(close):].strip()
            m["KU1_document"] += bool(text.strip()) and not complete_tags
        source = by_offset[p["cc"]["o"]]
        if p["cc"]["k"] == "0" and source.get("type") == "user":
            c = source["message"]["content"]
            if isinstance(c, list):
                c = next((b.get("text", "") for b in c if b.get("type") == "text"), "")
            if isinstance(c, str) and c.startswith("<command-name>/clear</command-name>") and opened:
                m["KT1_clear_while_open"] += 1
    for _, j in rows:
        if j.get("type") == "assistant" and j["message"].get("stop_reason") == "end_turn":
            mid = j["message"].get("id", j.get("uuid"))
            n = sum(b.get("type") == "text" for b in j["message"]["content"])
            message_counts[mid] = message_counts.get(mid, 0) + n
            m["KT2"] += max(n - 1, 0)
    m["repeated_end_turn_id_texts"] = sum(max(n - 1, 0) for n in message_counts.values())
    m["open_at_eof"] = int(opened)
    m["KP1c_rollout"] = decrease
    m["utterances"] = spoken
    m["T_bytes"], m["C_bytes"] = len(original), len(rollout)
    m["ratio"] = len(rollout) / len(original)
    return m


class Server:
    def __init__(self, projects, data, suffix):
        self.endpoint = f"@vrscc-test-{os.getpid()}-{suffix}"
        self.log = (WORK / f"daemon-{suffix}.log").open("w", encoding="utf-8")
        self.args = [str(DAEMON), "--projects", str(projects), "--data", str(data), "--endpoint", self.endpoint, "--workers", "6", "--connections", "16", "--timeout-ms", "30000"]
        self.proc = subprocess.Popen(self.args, stdout=self.log, stderr=self.log, env=dict(os.environ, VRS_CC_TEST_METRICS="1"))
        deadline = time.monotonic() + 5
        while True:
            try:
                with socket.socket(socket.AF_UNIX) as s:
                    s.connect("\0" + self.endpoint[1:])
                if "VRS_TEST_OPEN" not in Path(self.log.name).read_text(encoding="utf-8"):
                    if time.monotonic() > deadline:
                        raise RuntimeError("daemon open deadline")
                    time.sleep(0.02)
                    continue
                break
            except OSError:
                if self.proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("daemon start failed")
                time.sleep(0.02)

    def call(self, **request):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(45)
            s.connect("\0" + self.endpoint[1:])
            s.sendall((json.dumps(request, ensure_ascii=False) + "\n").encode())
            with s.makefile("rb") as f:
                response = json.loads(f.readline())
        return response

    def close(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(45)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.log.close()
        assert self.proc.returncode == 0, f"daemon did not shut down cleanly: {self.proc.returncode}"


def pages(server, transcript, budget):
    items, texts, start = [], [], None
    for _ in range(10000):
        args = dict(op="restore", transcript=str(transcript), budget=budget)
        if start is not None:
            args["start"] = start
        r = server.call(**args)
        assert r["ok"], r
        assert r["utf16"] == units(r["text"]) <= budget
        for fragment in r["items"]:
            key = (fragment["cc"]["o"], int(fragment["cc"]["k"]))
            c = fragment["c"]
            if items and key == (items[-1]["cc"]["o"], int(items[-1]["cc"]["k"])):
                assert c == units(items[-1]["text"]), (key, c)
                assert all(fragment[k] == items[-1][k] for k in ("cc", "role", "timestamp"))
                items[-1]["text"] += fragment["text"]
            else:
                assert c == 0 and (not items or key > (items[-1]["cc"]["o"], int(items[-1]["cc"]["k"])))
                items.append({k:v for k,v in fragment.items() if k != "c"})
        texts.append(r["text"])
        if r["next"] is None:
            return items, texts, r
        assert r["next"] != start
        start = r["next"]
    raise AssertionError("nonterminating pagination")


def main():
    samples = sorted(SAMPLES.rglob("*.jsonl"), key=lambda p: (-p.stat().st_size, str(p)))
    assert samples, "sample corpus must be nonempty"
    details["sample_manifest"] = {str(p.relative_to(SAMPLES)): hashlib.sha256(p.read_bytes()).hexdigest() for p in samples}
    report("CORE", "PASS" in run(BIN, "core"), "JSON/UTF16/SHA/C2")
    natives = {}
    all_outputs = {}
    for p in samples:
        base = WORK / p.stem
        try:
            result = json.loads(run(BIN, "determinism", p, base))
            natives[p.name] = result
            c = (base / "a" / p.name).read_bytes()
            all_outputs[p] = c
            report("U1:" + p.stem[:12], True, f"tails=50 threads=8 bytes={len(c)} sha256={hashlib.sha256(c).hexdigest()}")
            report("U4:" + p.stem[:12], True, "partial_original=absent completed=equal torn_rollout=equal")
        except Exception as e:
            report("U1/U4:" + p.stem[:12], False, str(e))
    for p in samples[:3]:
        try:
            out = WORK / p.stem / "parallel" / p.name
            run(BIN, "parallel", p, out)
            assert out.read_bytes() == all_outputs[p]
            report("U2:" + p.stem[:12], True, f"workers=8 source_bytes={p.stat().st_size} chunks_at_2MiB")
        except Exception as e:
            report("U2:" + p.stem[:12], False, str(e))
    total_cc = 0
    for p, c in all_outputs.items():
        try:
            original = p.read_bytes()
            offset = 0
            for expected_ord, raw in enumerate(c.splitlines(keepends=True)):
                assert raw.endswith(b"\n")
                j = rollout_load(raw)
                assert j["ordinal"] == expected_ord
                if expected_ord == 0:
                    assert j["payload"] == {"id": p.stem}
                else:
                    assert list(j["payload"])[-1] == "cc"
                    cc = j["payload"]["cc"]
                    orig = original[cc["o"]:cc["o"] + cc["n"]]
                    assert hashlib.sha256(orig).hexdigest() == cc["h"]
                    assert original[cc["o"] + cc["n"]:cc["o"] + cc["n"] + 1] == b"\n"
                    assert c[offset:offset + len(raw)] == raw
                    total_cc += 1
                offset += len(raw)
            assert offset == len(c)
            natives[p.name]["metrics"] = metrics(original, c)
            report("U3:" + p.stem[:12], True, f"cc_lines={expected_ord} bytes={offset}")
        except Exception as e:
            report("U3:" + p.stem[:12], False, repr(e))
    details["conversion"] = natives
    server = Server(SAMPLES, WORK / "sample-data", "sample")
    u6_samples = []
    try:
        for p in samples:
            try:
                items, texts, last = pages(server, p, 95000)
                original = p.read_bytes()
                boundary = None
                off = 0
                for raw in original.splitlines(keepends=True):
                    j = json.loads(raw)
                    if j.get("type") == "system" and j.get("subtype") == "compact_boundary":
                        boundary = off
                    off += len(raw)
                assert last["boundary"] == boundary
                assert bool(items) or boundary is None
                for item in items:
                    cc = item["cc"]
                    assert item["role"] in ("user", "assistant") and cc["o"] < boundary
                    raw = original[cc["o"]:cc["o"] + cc["n"]]
                    assert hashlib.sha256(raw).hexdigest() == cc["h"]
                    assert independent_body(json.loads(raw), int(cc["k"]), p.name, cc) == item["text"]
                sizes = [units(item["text"]) for item in items]
                details.setdefault("restore", {})[p.name] = {"items": len(items), "pages": len(texts), "utf16": sum(map(units, texts)), "boundary": boundary, "max_body_utf16": max(sizes, default=0), "bodies_over_4000": sum(n > 4000 for n in sizes)}
                report("U5:" + p.stem[:12], True, f"items={len(items)} pages={len(texts)} boundary={boundary} sha_body_mismatches=0")
                if boundary is not None and len(texts) == 1:
                    u6_samples.append((p, items))
            except Exception as e:
                report("U5:" + p.stem[:12], False, repr(e))
        for p, expected in u6_samples:
            try:
                items, texts, _ = pages(server, p, 4000)
                assert items == expected
                report("U6:" + p.stem[:12], True, f"items={len(items)} pages4000={len(texts)} pages95000=1")
            except Exception as e:
                report("U6:" + p.stem[:12], False, repr(e))
        if not u6_samples:
            report("U6:provided-sample", False, "no compacted sample fits a single 95000-unit page; fixture checked separately")
        # Static content must produce byte-identical responses, ten pairs.
        p = samples[-1]
        checks = [server.call(op="restore", transcript=str(p)) == server.call(op="restore", transcript=str(p)) for _ in range(10)]
        report("M5-repeat", all(checks), f"equal_pairs={sum(checks)}/10")
    finally:
        server.close()
    integration_fixture()
    definition_witness()
    details["cc_verified"] = total_cc


def definition_witness():
    directory = WORK / "zero-output-witness"
    directory.mkdir()
    t = directory / "source.jsonl"
    c = directory / "rollout.jsonl"
    first = (json.dumps({"type": "user", "timestamp": "2026-10-13T00:00:00.000Z", "uuid": "witness", "message": {"content": "utterance"}}, separators=(",", ":")) + "\n").encode()
    omitted = b'{"type":"queue-operation"}\n'
    observations = []
    for n in (100, 10000):
        with t.open("wb") as f:
            f.write(first + omitted * n)
        run(BIN, "conv", t, c)
        observation = json.loads(run(BIN, "tail", t, c))
        assert observation["source"] == len(first) + n * len(omitted) and observation["tail_bytes"] == 0
        observation["omitted_lines"] = n
        observations.append(observation)
    details["zero_output_witness"] = observations
    report("KZ1-zero-output", True, "zero_output_suffix=100/10000 e_T=EOF reread_tail_bytes=" + "/".join(str(x["tail_bytes"]) for x in observations))
    # A zero-output prefix without timestamp must not affect session_meta.
    empty_first = directory / "empty-first.jsonl"
    empty_first.write_bytes(omitted + first + omitted)
    run(BIN, "determinism", empty_first, directory / "empty-first")
    run(BIN, "parallel", empty_first, directory / "empty-first-parallel.jsonl")
    assert (directory / "empty-first/a/empty-first.jsonl").read_bytes() == (directory / "empty-first-parallel.jsonl").read_bytes()
    report("KZ1-empty-timestamp", True, "full/tails/concurrent/parallel byte_equal")


def integration_fixture():
    projects = WORK / "projects"
    project = projects / "test-project"
    project.mkdir(parents=True)
    sid = "11111111-2222-3333-4444-555555555555"
    t = project / (sid + ".jsonl")
    rows = []
    def add(typ, content, i, **extra):
        j = {"type": typ, "uuid": f"u{i}", "timestamp": f"2026-10-13T00:{i//60:02}:{i%60:02}.000Z"}
        if typ in ("user", "assistant"):
            j["message"] = {"role": typ, "content": content}
            if typ == "assistant":
                j["message"]["stop_reason"] = "end_turn"
        else:
            j["content"] = content
        j.update(extra)
        rows.append(j)
    for i in range(24):
        add("user", f"사실 {i}: 값🔑{i}. " + "한글😀 가나다라 " * 35, i*2)
        add("assistant", [{"type": "text", "text": f"답 {i}: 값🔑{i}. " + "보존 확인😀 " * 20}], i*2+1)
    add("system", "compacted", 48, subtype="compact_boundary")
    add("user", "사실 3을 다시 알려 줘", 49)
    with t.open("w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
    server = Server(projects, WORK / "fixture-data", "fixture")
    try:
        items, big, _ = pages(server, t, 95000)
        small_items, small, _ = pages(server, t, 4000)
        assert len(big) == 1 and len(small) > 1 and items == small_items
        report("U6:fixed-fixture", True, f"items={len(items)} pages4000={len(small)} pages95000=1 max_utf16={max(map(units,small))}")
        data = t.read_bytes()
        restored = bytearray()
        start = 0
        read_pages = 0
        while start < len(data):
            r = server.call(op="read", path=str(t), start=start, end=len(data), budget=4000)
            assert r["ok"], r
            b = r["bytes"].encode()
            assert r["sha256"] == hashlib.sha256(b).hexdigest() and units(r["text"]) <= 4000
            restored.extend(b)
            read_pages += 1
            if r["next"] is None:
                break
            assert r["next"] > start
            start = r["next"]
        assert bytes(restored) == data
        checks = [server.call(op="read", path=str(ROOT / "native/libvrs/vrs.h"), start=0, end=1).get("error") == "READ_OUTSIDE_ROOT",
                  not server.call(op="restore", transcript=str(t), start="0:999")["ok"],
                  not server.call(op="restore", transcript=str(t), budget=95001)["ok"],
                  not server.call(op="read", path=str(t), start=10, end=2)["ok"]]
        report("READ/errors", all(checks), f"read_pages={read_pages} byte_equal=1 rejection_checks={sum(checks)}/4")
        same = server.call(op="restore", transcript=str(t))
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            rs = list(pool.map(lambda _: server.call(op="restore", transcript=str(t)), range(16)))
        report("REQUEST-concurrency", all(r == same for r in rs), f"equal={sum(r == same for r in rs)}/16 callers=8")
        success = 0
        for i in range(100):
            row = {"type": "user", "uuid": f"live{i}", "timestamp": f"2026-10-13T01:{i//60:02}:{i%60:02}.000Z", "message": {"role": "user", "content": f"uniqueword_{i}"}}
            encoded = (json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
            off = t.stat().st_size
            with t.open("ab") as f:
                f.write(encoded)
            r = server.call(op="diag_assemble", transcript=str(t))
            assert r["ok"], r
            success += any(c["o"] == off and c["h"] == hashlib.sha256(encoded[:-1]).hexdigest() for c in r["users"])
        report("M4-local", success == 100, f"new_user_present={success}/100")
        bridge_checks(server, projects, t, sid)
        before_restart = server.call(op="restore", transcript=str(t))
        server.close()
        began = time.monotonic()
        server = Server(projects, WORK / "fixture-data", "fixture-restart")
        after_restart = server.call(op="restore", transcript=str(t))
        report("RESTART", before_restart == after_restart, f"equal_response={int(before_restart == after_restart)} elapsed_s={time.monotonic()-began:.3f}")
        # Authentication failure is checked on a copy, preserving all samples.
        changed = t.read_bytes().replace("사실 0".encode(), "사실 X".encode(), 1)
        with t.open("wb") as f:
            f.write(changed)
        r = server.call(op="restore", transcript=str(t))
        report("SHA-tamper", not r["ok"] and "SHA_MISMATCH" in r.get("error", ""), str(r)[:180])
    finally:
        server.close()


def bridge_checks(server, projects, transcript, sid):
    bridge = ROOT / "src/bridge/vrs_lib_bridge.py"
    spec = importlib.util.spec_from_file_location("vrs_bridge_test", bridge)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    session = WORK / "test-session.json"
    record = {"pid": os.getpid(), "sessionId": sid, "procStart": module.linux_process(os.getpid())[1]}
    session.write_text(json.dumps(record) + "\n", encoding="utf-8")
    env = dict(os.environ, VRS_CC_PROJECTS=str(projects), VRS_CC_DATA=str(WORK / "fixture-data"), VRS_CC_ENDPOINT=server.endpoint, VRS_CC_TEST_SESSION_FILE=str(session))
    calls = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-03-26"}},
             {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
             {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "vrs_restore", "arguments": {}}}]
    p = subprocess.run([sys.executable, str(bridge)], input="".join(json.dumps(c) + "\n" for c in calls), encoding="utf-8", capture_output=True, env=env, check=True)
    rs = [json.loads(x) for x in p.stdout.splitlines()]
    tools = rs[1]["result"]["tools"]
    assert len(tools) == 2 and all(tool["_meta"]["anthropic/maxResultSizeChars"] == 100000 for tool in tools)
    assert not rs[2]["result"]["isError"] and sid in rs[2]["result"]["content"][0]["text"]
    record["procStart"] = "0"
    session.write_text(json.dumps(record) + "\n", encoding="utf-8")
    p = subprocess.run([sys.executable, str(bridge)], input=json.dumps(calls[-1])+"\n", encoding="utf-8", capture_output=True, env=env, check=True)
    assert json.loads(p.stdout)["result"]["isError"]
    env.pop("VRS_CC_TEST_SESSION_FILE")
    p = subprocess.run([sys.executable, str(bridge)], input=json.dumps(calls[-1])+"\n", encoding="utf-8", capture_output=True, env=env, check=True)
    assert json.loads(p.stdout)["result"]["isError"]
    report("MCP-local", True, "tools=2 meta=100000 caller_test_override=verified procStart_mismatch=rejected no_identity=rejected U7_live=not_run")
    old_env = dict(os.environ)
    auto_endpoint = f"@vrscc-auto-{os.getpid()}"
    os.environ.update(VRS_CC_PROJECTS=str(projects), VRS_CC_DATA=str(WORK / "auto-data"), VRS_CC_ENDPOINT=auto_endpoint, VRS_CC_DAEMON=str(DAEMON))
    client = module.DaemonClient()
    try:
        began = time.monotonic()
        r = client.call({"op": "restore", "transcript": str(transcript)})
        elapsed = time.monotonic() - began
        assert r["ok"] and client.spawned is not None and elapsed < 5
        duplicate = subprocess.run([str(DAEMON), "--projects", str(projects), "--data", str(WORK / "auto-data"), "--endpoint", auto_endpoint], capture_output=True, encoding="utf-8", timeout=5)
        assert duplicate.returncode != 0 and "ALREADY_RUNNING" in duplicate.stderr
        report("AUTOSTART/singleton", True, f"detached_pid={client.spawned.pid} elapsed_s={elapsed:.3f} second_daemon_rejected=1")
    finally:
        if client.spawned is not None:
            client.spawned.terminate()
            client.spawned.wait(45)
        os.environ.clear()
        os.environ.update(old_env)


try:
    main()
except Exception as exc:
    import traceback
    traceback.print_exc()
    report("HARNESS", False, repr(exc))
finally:
    details["work_directory"] = str(WORK)
    details["criteria"] = "tests/PLAN3.md supersedes pagination and zero-output; sample subset only"
    (OUT / "u_details.json").write_text(json.dumps(details, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (OUT / "u_results.txt").write_text("\n".join(results) + f"\nARTIFACTS {WORK}\n", encoding="utf-8")
sys.exit(any(" FAIL " in line for line in results))
