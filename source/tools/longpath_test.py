#!/usr/bin/env python3
"""KL3 Windows relay: fixed samples, M2, byte equality, actual OS overflow.

Run with --daemon EXE --cli EXE --work NEW_DIR. Python is only an independent
observer/fixture writer; the daemon and CLI receive ordinary unprefixed paths.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import traceback

from g3_writer_test import publish, rpc
from kwov_test import line

STAGE = Path(r'<mcp>\vrs-lib\stage')
PROJECT = '<project-dir>'
SAMPLES = {
    '3ef9dae3-a341-433f-a88b-5af6638cb92d.jsonl': 66,
    '845221e9-2439-4250-9d97-d8379559740a.jsonl': 0,
    '99451ea7-1d67-4707-aa2a-1a365c24e255.jsonl': 167,
    '99451ea7-1d67-4707-aa2a-1a365c24e255/subagents/agent-acb21828acc14ffa3.jsonl': 17,
}
STRIP_US = re.compile(rb',"us":\d+\}$')


def io(path):
    """Python fixture I/O also works when LongPathsEnabled is zero."""
    s = os.path.abspath(path)
    if s.startswith(('\\\\?\\', '\\\\.\\')):
        return Path(s)
    return Path('\\\\?\\UNC\\' + s[2:] if s.startswith('\\\\') else '\\\\?\\' + s)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def units(s):
    return len(s.encode('utf-16-le')) // 2


def no_namespace(value):
    if isinstance(value, dict):
        for k, v in value.items():
            no_namespace(k)
            no_namespace(v)
    elif isinstance(value, list):
        for v in value:
            no_namespace(v)
    elif isinstance(value, str):
        assert '\\\\?\\' not in value and '//?/' not in value, value[:200]


def covered(path):
    try:
        b = io(path).read_bytes()
    except FileNotFoundError:
        return -1
    return max(struct.unpack('<64Q', b)) if len(b) == 512 else -1


def body(j, c, name):
    k = int(c['k'])
    content = j['attachment']['prompt'] if j['type'] == 'attachment' else j['message']['content']
    if isinstance(content, str):
        assert k == 0
        return content
    b = content[k]
    if b['type'] == 'image':
        return f"[image {b['source']['media_type']} · {name}#{c['o']}+{c['n']} k={c['k']}]"
    assert b['type'] == 'text'
    return b['text']


def restore(endpoint, path, expected):
    original = io(path).read_bytes()
    boundary, off = None, 0
    for raw in original.splitlines(keepends=True):
        j = json.loads(raw)
        if j.get('type') == 'system' and j.get('subtype') == 'compact_boundary':
            boundary = off
        off += len(raw)
    items, replies, start = [], [], None
    for _ in range(10000):
        req = dict(op='restore', transcript=str(path), budget=95000)
        if start is not None:
            req['start'] = start
        r = rpc(endpoint, req)
        no_namespace(r)
        assert r['total'] == expected and r['boundary'] == boundary, r
        assert r['utf16'] == units(r['text']) <= 95000
        replies.append(r)
        for f in r['items']:
            key = (f['cc']['o'], int(f['cc']['k']))
            if items and key == (items[-1]['cc']['o'], int(items[-1]['cc']['k'])):
                assert f['c'] == units(items[-1]['text'])
                assert all(f[k] == items[-1][k] for k in ('cc', 'role', 'timestamp'))
                items[-1]['text'] += f['text']
            else:
                assert f['c'] == 0
                assert not items or key > (items[-1]['cc']['o'], int(items[-1]['cc']['k']))
                items.append({k: v for k, v in f.items() if k != 'c'})
        if r['next'] is None:
            break
        assert r['next'] != start
        start = r['next']
    else:
        raise AssertionError('pagination did not terminate')
    assert len(items) == expected
    for item in items:
        c = item['cc']
        assert c['o'] < boundary and c['o'] + c['n'] <= len(original)
        raw = original[c['o']:c['o'] + c['n']]
        assert sha(raw) == c['h']
        j = json.loads(raw)
        assert item['role'] == ('user' if j['type'] == 'attachment' else j['type'])
        assert body(j, c, path.name).encode('utf-8') == item['text'].encode('utf-8')
    return replies


class Daemon:
    def __init__(self, exe, projects, data, work, label, overflow=False):
        self.endpoint = r'\\.\pipe\vrs-longpath-' + str(os.getpid()) + '-' + label
        self.logpath = work / (label + '.log')
        self.log = self.logpath.open('wb')
        self.writers = data / 'writers'
        env = dict(os.environ, VRS_CC_TEST_METRICS='1')
        if overflow:
            env['VRS_CC_TEST_WATCH_BYTES'] = '64'
        # Exercise dot components at the actual API boundary, not just in probe.
        self.proc = subprocess.Popen([str(exe), '--projects', str(projects) + '/./',
            '--data', str(data) + '/./', '--writer-markers', str(self.writers),
            '--endpoint', self.endpoint, '--workers', '6', '--connections', '4'],
            env=env, stdout=subprocess.DEVNULL, stderr=self.log)

    def logs(self):
        return self.logpath.read_text(encoding='utf-8', errors='replace')

    def wait(self, predicate, label):
        end = time.monotonic() + 120
        while time.monotonic() < end:
            assert self.proc.poll() is None, self.logs()[-4000:]
            if predicate():
                return
            time.sleep(.025)  # Observer polling only; no new production wait.
        raise AssertionError(label + ': ' + self.logs()[-4000:])

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=30)
        self.log.close()


def cli_check(cli, projects, data, work, label):
    listing = work / (label + '-list.txt')
    listing.write_text(''.join(PROJECT + '/' + s + '\n' for s in SAMPLES), encoding='utf-8')
    results = []
    for mode in ('batch', 'reopen'):
        output = work / (label + '-' + mode)
        output.mkdir()
        p = subprocess.run([str(cli), '--stats', '--workers', '6', '--out', str(output),
            str(data / 'cc_rollout'), str(data / 'cli-idx'), '@' + str(listing)],
            capture_output=True, timeout=180)
        (work / (label + '-' + mode + '.log')).write_bytes(p.stdout + p.stderr)
        assert p.returncode == 0, (p.returncode, p.stdout[-1000:], p.stderr[-1000:])
        assert re.findall(rb'^assemble rc=(\d+)', p.stdout, re.M) == [b'0'] * 4
        files = {p.name: STRIP_US.sub(b'}', p.read_bytes()) for p in output.iterdir()}
        assert len(files) == 4
        for raw in files.values():
            no_namespace(json.loads(raw))
        digest = re.search(rb'cells_hash=([0-9a-f]+)', p.stdout).group(1)
        results.append((files, digest))
    assert results[0] == results[1], 'batch/reopen differ'
    return results[0]


def run(args, summary):
    work = args.work.absolute()
    work.mkdir(parents=True, exist_ok=False)
    deep = Path(*(['long-' + 'd' * 70] * 4))
    probe = work / 'probe' / deep
    p = subprocess.run([str(args.daemon), '--test-longpath-fs', str(probe)], capture_output=True, timeout=60)
    (work / 'filesystem-probe.log').write_bytes(p.stdout + p.stderr)
    summary['filesystem'] = json.loads(p.stdout)
    print('FILESYSTEM ' + json.dumps(summary['filesystem'], ensure_ascii=True), flush=True)
    assert p.returncode == 0 and summary['filesystem']['pass'], p.stderr
    roots, snapshots, assemblies = {}, {}, {}
    summary['fixtures'] = {}
    for label in ('long', 'short'):
        base = work / label
        projects = base / ('source' if label == 'short' else 'source-long')
        data = base / 'data'
        if label == 'long':
            projects /= deep
            data /= deep
        for rel in SAMPLES:
            source = STAGE / 'proj' / PROJECT / rel
            dest = projects / PROJECT / rel
            raw = source.read_bytes()
            io(dest.parent).mkdir(parents=True, exist_ok=True)
            io(dest).write_bytes(raw)
            assert io(dest).read_bytes() == raw
            summary['fixtures'][rel] = {'source': str(source), 'sha256': sha(raw), 'bytes': len(raw)}
            if label == 'long':
                assert len(str(dest)) > 300
                for sub, suffix in [('cc_rollout', ''), ('cover', '.cv'), ('idx', '.vx')]:
                    assert len(str(data / sub / PROJECT / (rel + suffix))) > 300
        roots[label] = (projects, data)
        server = Daemon(args.daemon, projects, data, work, label)
        try:
            server.wait(lambda: 'VRS_TEST_OPEN' in server.logs() and 'VRS_TEST_WATCH_READY' in server.logs(), 'startup')
            # KL1 intentionally returns NOT_READY until lib7 is ready. The test
            # observer retries only that answer; all other failures stay failures.
            def ready():
                try:
                    rpc(server.endpoint, dict(op='diag_assemble', transcript=str(projects / PROJECT / next(iter(SAMPLES)))))
                    return True
                except AssertionError as e:
                    if 'NOT_READY' in str(e):
                        return False
                    raise
            server.wait(ready, 'lib7 readiness')
            replies = {rel: restore(server.endpoint, projects / PROJECT / rel, want) for rel, want in SAMPLES.items()}
            ct = {rel: io(data / 'cc_rollout' / PROJECT / rel).read_bytes() for rel in SAMPLES}
            for rel, raw in ct.items():
                assert raw == (STAGE / 'data5' / 'cc_rollout' / PROJECT / rel).read_bytes()
                assert covered(data / 'cover' / PROJECT / (rel + '.cv')) == io(projects / PROJECT / rel).stat().st_size
            snapshots[label] = (replies, ct)
            # read exposes the canonical path: verify no prefix leakage and bytes.
            target = projects / PROJECT / next(iter(SAMPLES))
            raw = io(target).read_bytes().split(b'\n', 1)[0] + b'\n'
            r = rpc(server.endpoint, dict(op='read', path=str(target.parent) + '/./../' + target.parent.name + '/' + target.name, start=0, end=len(raw), budget=95000))
            no_namespace(r)
            assert r['bytes'].encode() == raw and r['sha256'] == sha(raw)
            assert 'conversion:' not in server.logs() and 'watch:' not in server.logs()
        finally:
            server.stop()
        assemblies[label] = cli_check(args.cli, projects, data, work, label)
        summary[label] = {'restore_counts': list(SAMPLES.values()), 'm2_items': 250,
            'min_source_length': min(len(str(projects / PROJECT / r)) for r in SAMPLES),
            'min_derived_length': min(len(str(data / 'cover' / PROJECT / (r + '.cv'))) for r in SAMPLES),
            'cells_hash': assemblies[label][1].decode(), 'pass': True}
        print(label.upper() + ' ' + json.dumps(summary[label]), flush=True)
    assert snapshots['long'] == snapshots['short'], 'restore or C(T) bytes differ'
    # Only permitted path substitution; no JSON reserialization or field removal.
    def normalize(raw, label):
        projects, data = roots[label]
        for value, token in [(projects, '$PROJECTS'), (data, '$DATA')]:
            for s in (str(value).replace('\\', '/'), str(value)):
                raw = raw.replace(json.dumps(s, ensure_ascii=False)[1:-1].encode(), token.encode())
        return raw
    a, b = assemblies['long'], assemblies['short']
    assert a[1] == b[1]
    assert a[0].keys() == b[0].keys()
    assert all(normalize(a[0][k], 'long') == normalize(b[0][k], 'short') for k in a[0]), 'assembly bytes differ'
    summary['byte_equality'] = True

    projects, data = roots['long']
    server = Daemon(args.daemon, projects, data, work, 'overflow', overflow=True)
    try:
        server.wait(lambda: 'VRS_TEST_OPEN' in server.logs() and 'VRS_TEST_WATCH_READY' in server.logs(), 'overflow startup')
        files = []
        for i in range(4):
            path = projects / 'new-한글' / ('session-' + 'x' * 96 + f'-{i}.jsonl')
            io(path.parent).mkdir(parents=True, exist_ok=True)
            # Publish W before source creation, also measuring weakly_canonical
            # with a nonexisting leaf below a long parent.
            publish(args.daemon, server.writers, path, os.getpid())
            io(path).write_bytes(line(i, 0))
            files.append(path)
        def recovered():
            return all(covered(data / 'cover' / (str(p.relative_to(projects)) + '.cv')) == io(p).stat().st_size for p in files)
        server.wait(recovered, 'new long sources')
        before = server.logs().count('VRS_TEST_OVERFLOW')
        for i in range(128):
            io(projects / ('noise-' + 'y' * 96 + f'-{i}.tmp')).touch()
        for i, p in enumerate(files):
            with io(p).open('ab') as f:
                f.write(b''.join(line(i, k) for k in range(1, 5)))
        server.wait(lambda: server.logs().count('VRS_TEST_OVERFLOW') > before and 'VRS_TEST_RESCAN' in server.logs() and recovered(), 'actual overflow recovery')
        # A later append proves the live long-name subscription still works.
        for i, p in enumerate(files):
            with io(p).open('ab') as f:
                f.write(line(i, 5))
        server.wait(recovered, 'post-overflow changes')
        total = 0
        for p in files:
            original, expected, off = io(p).read_bytes(), {}, 0
            for raw in original.splitlines(keepends=True):
                expected[off] = (raw[:-1], json.loads(raw)['message']['content'])
                off += len(raw)
            seen = set()
            for raw in io(data / 'cc_rollout' / p.relative_to(projects)).read_bytes().splitlines():
                row = json.loads(re.sub(rb'("ordinal":)0*(\d+)', rb'\1\2', raw, count=1))['payload']
                no_namespace(row)
                if row.get('type') != 'message':
                    continue
                c = row['cc']; source, text = expected[c['o']]
                assert c['n'] == len(source) and c['h'] == sha(source)
                assert row['content'][0]['text'].encode() == text.encode() and c['o'] not in seen
                seen.add(c['o']); total += 1
            assert seen == expected.keys()
        assert total == 24
        assert 'conversion:' not in server.logs() and 'watch:' not in server.logs()
        summary['overflow'] = {'pass': True, 'restore_calls': 0, 'message_lines': total,
            'count': server.logs().count('VRS_TEST_OVERFLOW'), 'buffer_bytes': 64}
    finally:
        server.stop()
    summary['pass'] = True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--daemon', required=True, type=Path)
    ap.add_argument('--cli', required=True, type=Path)
    ap.add_argument('--work', required=True, type=Path)
    args = ap.parse_args()
    if os.name != 'nt':
        ap.error('Windows only; no skipped-test success')
    args.daemon, args.cli = args.daemon.resolve(), args.cli.resolve()
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Control\FileSystem') as key:
        try:
            longpaths = winreg.QueryValueEx(key, 'LongPathsEnabled')[0]
        except FileNotFoundError:
            longpaths = 0
    summary = {'pass': False, 'LongPathsEnabled': longpaths}
    try:
        run(args, summary)
    except Exception as e:
        summary['error'] = repr(e)
        traceback.print_exc()
    if args.work.exists():
        (args.work / 'summary.json').write_text(json.dumps(summary, ensure_ascii=True, indent=2) + '\n', encoding='utf-8')
    print('LONGPATH ' + json.dumps(summary, ensure_ascii=True), flush=True)
    compact = {k: v for k, v in summary.items() if k not in ('fixtures', 'filesystem')}
    compact['filesystem'] = {k: ({key: (str(val)[:150] if key == 'error' else val) for key, val in v.items()} if isinstance(v, dict) else v) for k, v in summary.get('filesystem', {}).items() if k != 'path'}
    print('LONGPATH_RESULT ' + json.dumps(compact, ensure_ascii=True), flush=True)
    return 0 if summary['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
