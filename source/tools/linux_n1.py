#!/usr/bin/env python3
"""Run the fixed Linux N1 corpus comparison without changing its oracle.

Run under taskset -c 0-11. Every invocation needs a new --out directory.
The CLI's exit 1 is expected when the frozen assemble rc list contains rc 2.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


STRIP_US = re.compile(rb',"us":\d+\}$')
ASSEMBLE = re.compile(r'^assemble rc=(-?\d+) bytes=\d+ us=\d+ (.*)$', re.M)
STAT_KEYS = ('items', 'turns', 'sessions', 'cells_n', 'cells_hash')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stats(log):
    line = next((s for s in log.splitlines() if s.startswith('stats ')), '')
    return dict(re.findall(r'(\w+)=(\S+)', line))


def compare(binary, refs, sessions, out, workers):
    out.mkdir(parents=True, exist_ok=False)
    lines = (refs / 'list.txt').read_text(encoding='utf-8').splitlines()
    result = {'input_count': len(lines), 'list_sha256': sha(refs / 'list.txt')}
    for mode in ('batch', 'reopen'):
        dest = out / mode
        dest.mkdir()
        cmd = [str(binary), '--stats', '--workers', str(workers), '--cpus', '0-11',
               '--out', str(dest), str(sessions), str(out / 'index'),
               '@' + str(refs / 'list.txt')]
        proc = subprocess.run(cmd, capture_output=True, timeout=1800)
        log = proc.stdout.decode('utf-8')
        (out / (mode + '.log')).write_bytes(proc.stdout)
        (out / (mode + '.stderr')).write_bytes(proc.stderr)
        oracle = (refs / (mode + '.log')).read_text(encoding='utf-8')
        actual = ASSEMBLE.findall(log)
        expected = ASSEMBLE.findall(oracle)
        actual_rc = [int(rc) for rc, _ in actual]
        expected_rc = [int(rc) for rc, _ in expected]
        path_ok = len(actual) == len(lines) == len(expected) and all(
            got == str(sessions / rel)
            for (_, got), rel in zip(actual, lines))
        expected_names = {p.name for p in (refs / mode).iterdir() if p.is_file()}
        actual_names = {p.name for p in dest.iterdir() if p.is_file()}
        different = []
        for name in sorted(expected_names & actual_names):
            if STRIP_US.sub(b'}', (refs / mode / name).read_bytes()) != STRIP_US.sub(b'}', (dest / name).read_bytes()):
                different.append(name)
        mine, theirs = stats(log), stats(oracle)
        stat_equal = bool(mine) and all(k in mine and mine[k] == theirs.get(k) for k in STAT_KEYS)
        ready = re.search(r'^ready=1 .*ready_ms=([\d.]+)$', log, re.M)
        expected_exit = 1 if any(expected_rc) else 0
        row = {'command': cmd, 'process_rc': proc.returncode, 'expected_process_rc': expected_exit,
               'assemble_count': len(actual), 'rc_list_equal': actual_rc == expected_rc,
               'input_order_equal': path_ok, 'stats_equal': stat_equal,
               'stats': {k: mine.get(k) for k in STAT_KEYS},
               'reference_stats': {k: theirs.get(k) for k in STAT_KEYS},
               'missing_json': sorted(expected_names - actual_names),
               'extra_json': sorted(actual_names - expected_names), 'different_json': different,
               'json_count': len(actual_names), 'ready_ms': float(ready.group(1)) if ready else None,
               'reference_log_sha256': sha(refs / (mode + '.log')),
               'log_sha256': sha(out / (mode + '.log'))}
        row['pass'] = (proc.returncode == expected_exit and ready is not None and path_ok
                       and row['rc_list_equal'] and stat_equal and not different
                       and expected_names == actual_names)
        result[mode] = row
        print(json.dumps({'corpus': refs.name, 'mode': mode, 'pass': row['pass'],
                          'ready_ms': row['ready_ms'], 'json_count': row['json_count'],
                          'cells_hash': mine.get('cells_hash')}, ensure_ascii=False), flush=True)
    result['pass'] = all(result[m]['pass'] for m in ('batch', 'reopen'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--refs', type=Path, default=Path.home() / 'tmp/vrsnative/relay/ref')
    parser.add_argument('--m1', type=Path, default=Path.home() / 'tmp/m1/sessions')
    parser.add_argument('--kx2', type=Path, default=Path.home() / 'tmp/kx2/s')
    parser.add_argument('--workers', type=int, default=6)
    parser.add_argument('--source-rev', help='commit that built the binary; never inferred from the checker location')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    if args.source_rev:
        resolved = subprocess.run(['git', 'rev-parse', '--verify', args.source_rev + '^{commit}'],
                                  cwd=repo, text=True, capture_output=True)
        if resolved.returncode:
            parser.error('--source-rev must name an existing commit in this repository')
        args.source_rev = resolved.stdout.strip()
    if not set(os.sched_getaffinity(0)) <= set(range(12)):
        parser.error('run with taskset -c 0-11')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    summary = {'binary': str(binary), 'binary_sha256': sha(binary),
               'rev': args.source_rev,
               'checker_rev': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
               'affinity': sorted(os.sched_getaffinity(0)), 'workers': args.workers}
    for name in ('m1', 'kx2'):
        summary[name] = compare(binary, (args.refs / name).resolve(),
                                getattr(args, name).absolute(), args.out / name, args.workers)
        (args.out / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    summary['pass'] = all(summary[n]['pass'] for n in ('m1', 'kx2'))
    (args.out / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return 0 if summary['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
