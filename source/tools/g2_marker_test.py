#!/usr/bin/env python3
"""Cooperative P-contract N5; never substitutes for the unmarked kwov case."""
import ctypes
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time

from kwov_test import line, cover, HandleSamples


def suspend(proc, paused):
    if os.name == 'nt':
        fn = ctypes.windll.ntdll.NtSuspendProcess if paused else ctypes.windll.ntdll.NtResumeProcess
        fn.argtypes = [ctypes.c_void_p]
        fn.restype = ctypes.c_long
        assert fn(int(proc._handle)) == 0
    else:
        os.kill(proc.pid, signal.SIGSTOP if paused else signal.SIGCONT)


def run_case(daemon, work, count):
    work.mkdir(parents=True)
    projects, data, markers = work / 'projects', work / 'data', work / 'marks'
    projects.mkdir(); markers.mkdir()
    if os.name != 'nt':
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if soft > 4096:
            resource.setrlimit(resource.RLIMIT_NOFILE, (4096, hard))
        assert resource.getrlimit(resource.RLIMIT_NOFILE)[0] == 4096
    files = []
    for i in range(count):
        p = projects / f'{i:05}.jsonl'
        p.write_bytes(line(i, 0) if i < 32 else b'{"type":"progress"}\n')
        files.append(p)
    endpoint = ('\\\\.\\pipe\\vrs-g2-' if os.name == 'nt' else '@vrs-g2-') + str(os.getpid())
    logpath = work / 'daemon.log'
    times = []

    def publish(path, hook=False):
        began = time.perf_counter_ns()
        cmd = [str(daemon), '--mark-hook', str(markers)] if hook else [str(daemon), '--mark-root', str(markers), '--mark-path', str(path)]
        p = subprocess.run(cmd, input=json.dumps({'transcript_path': str(path)}).encode() if hook else None,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        assert p.returncode == 0, p.stderr
        assert not p.stdout
        times.append((time.perf_counter_ns() - began) / 1000)

    def covered():
        return all(cover(data / 'cover' / (str(p.relative_to(projects)) + '.cv')) == p.stat().st_size for p in files)

    def validate():
        n = 0
        for p in files:
            expected, offset = {}, 0
            for raw in p.read_bytes().splitlines(keepends=True):
                row = json.loads(raw)
                if row.get('type') == 'user':
                    expected[offset] = (raw[:-1], row['message']['content'])
                offset += len(raw)
            actual = set()
            converted = data / 'cc_rollout' / p.relative_to(projects)
            if expected:
                for raw in converted.read_bytes().splitlines():
                    payload = json.loads(re.sub(rb'("ordinal":)0*(\d+)', rb'\1\2', raw, count=1))['payload']
                    if payload.get('type') != 'message':
                        continue
                    c = payload['cc']; original, body = expected[c['o']]
                    assert c['n'] == len(original) and c['h'] == hashlib.sha256(original).hexdigest()
                    assert payload['content'][0]['text'] == body and c['o'] not in actual
                    actual.add(c['o']); n += 1
            assert actual == set(expected)
        return n

    env = dict(os.environ, VRS_CC_TEST_METRICS='1', VRS_CC_TEST_WATCH_BYTES='64', VRS_CC_TEST_MARKERS_ONLY='1')
    with logpath.open('wb') as log:
        proc = subprocess.Popen([str(daemon), '--projects', str(projects), '--data', str(data),
                                 '--endpoint', endpoint, '--workers', '6', '--connections', '4',
                                 '--change-markers', str(markers) + os.sep], stderr=log, stdout=subprocess.DEVNULL, env=env)
        monitor = HandleSamples(proc)
        paused = False

        def logs():
            return logpath.read_text(encoding='utf-8', errors='replace')

        def wait(predicate, label, timeout=120):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                assert proc.poll() is None, logs()[-3000:]
                if predicate(): return
                time.sleep(.01)
            raise AssertionError(label + '\n' + logs()[-4000:])

        try:
            wait(lambda: 'VRS_TEST_OPEN' in logs() and 'VRS_TEST_WATCH_READY' in logs() and covered(), 'startup')
            suspend(proc, True); paused = True
            capacity = 128 if os.name == 'nt' else int(Path('/proc/sys/fs/inotify/max_queued_events').read_text()) + 64
            assert capacity <= 1048640
            # Noise exists only while the owned child is paused. It consumes the
            # actual OS buffer, but is absent from recovery enumeration.
            for i in range(capacity):
                p = markers / f'noise-{i}.tmp'; p.touch(); p.unlink()
            changed = files[:32] + [files[-1]]
            for i, p in enumerate(changed):
                with p.open('ab') as f: f.write(line(i, 1))
                publish(p, hook=True)
            nested = projects / 'new' / 'nested'; nested.mkdir(parents=True)
            for i in range(4):
                p = nested / f'new-{i}.jsonl'; p.write_bytes(line(100 + i, 1))
                files.append(p); publish(p, hook=True)
            assert len(list(markers.glob('*.mark'))) == 37
            before = time.perf_counter_ns()
            suspend(proc, False); paused = False
            wait(lambda: 'VRS_TEST_OVERFLOW' in logs() and covered() and not list(markers.iterdir()), 'marker overflow recovery')
            recovered_us = (time.perf_counter_ns() - before) / 1000
            recovered = validate()
            rows = [(int(a), int(b)) for a, b in re.findall(r'VRS_TEST_MARKERS entries=(\d+) us=(\d+)', logs())]
            # The enumerator visits precisely the dirty set, independent of N.
            assert max(n for n, _ in rows) == 37, rows
            for i, p in enumerate((files[0], files[-1])):
                with p.open('ab') as f: f.write(line(200 + i, 2))
                publish(p)
            wait(lambda: covered() and not list(markers.iterdir()), 'ordinary marker cleanup')
            final = validate()
            # An incomplete line must keep its marker until the rest arrives.
            p = files[0]; raw = line(300, 3)
            with p.open('ab') as f: f.write(raw[:-1])
            publish(p)
            time.sleep(.1)
            assert len(list(markers.glob('*.mark'))) == 1
            with p.open('ab') as f: f.write(raw[-1:])
            publish(p)
            wait(lambda: covered() and not list(markers.iterdir()), 'partial line completion')
            assert validate() == final + 1
            # Concurrent publishers overlap the actual daemon's six cleanup
            # workers. Source appends themselves use one test writer, so the
            # oracle never relies on platform-specific multi-writer append.
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                futures = []
                for i in range(64):
                    with files[0].open('ab') as f: f.write(line(400 + i, 4 + i))
                    futures.append(pool.submit(publish, files[0], True))
                for f in futures: f.result()
            wait(lambda: covered() and not list(markers.iterdir()), 'concurrent publication/cleanup')
            assert validate() == final + 65
            errors = [s for s in logs().splitlines() if 'conversion:' in s or 'watch:' in s]
            assert not errors, logs()[-2500:] + '\nALL_ERRORS: ' + repr(errors)
            result = dict(pass_=True, initial_files=count, changed_files=37, recovered_messages=recovered,
                          final_messages=final + 65, concurrent_publications=64, marker_scans=rows, recovered_us=recovered_us,
                          hook_process_median_us=statistics.median(times[:37]),
                          hook_process_max_us=max(times[:37]), overflow_count=logs().count('VRS_TEST_OVERFLOW'),
                          remaining_markers=0, restore_calls=0)
        finally:
            handles = monitor.finish(work)
            if paused: suspend(proc, False)
            proc.terminate()
            try: proc.wait(timeout=30)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    result['handles'] = handles
    result['pass'] = result.pop('pass_') and handles['pass']
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def run_suite(daemon, work):
    rows = [run_case(daemon.resolve(), work / str(n), n) for n in (64, 4128)]
    return {'pass': all(r['pass'] for r in rows), 'writer_contract': 'post-append publish P', 'cases': rows}


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(); ap.add_argument('--daemon', type=Path, required=True); ap.add_argument('--work', type=Path, required=True)
    args = ap.parse_args()
    print(json.dumps(run_suite(args.daemon, args.work.resolve())))
