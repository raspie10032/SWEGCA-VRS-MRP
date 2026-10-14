#!/usr/bin/env python3
"""N4: relay-compatible product audit, with explicit allocator/runtime boundaries."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SYMBOLS = re.compile(r'libat_|__atomic_\w*_16|pthread_mutex|pthread_cond|__wait_impl|__notify_impl|__emutls|EnterCriticalSection|AcquireSRWLock|SleepConditionVariable')
LOCKS = re.compile(r'std::mutex|lock_guard|unique_lock|scoped_lock|condition_variable|std::queue|std::deque|CRITICAL_SECTION|SRWLOCK|std::shared_mutex')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--platform', choices=('linux', 'windows'), default='windows' if os.name == 'nt' else 'linux')
    ap.add_argument('--build', type=Path)
    ap.add_argument('--nm', default='nm')
    ap.add_argument('--output', type=Path)
    a = ap.parse_args()
    build = a.build or ROOT / ('src/build-windows' if a.platform == 'windows' else 'src/build')
    out = a.output or Path(os.environ.get('VRS_CC_TEST_OUTPUT', str(build / 'audit')))
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((build / 'build_manifest.json').read_text(encoding='utf-8'))
    if manifest['platform'] != a.platform or not manifest['objects'] or not manifest['executables']:
        raise RuntimeError('Missing objects/executables or mismatched build platform')
    sources = sorted(p for folder in (ROOT / 'native/libvrs', ROOT / 'src')
                     for p in folder.iterdir() if p.suffix in ('.cpp', '.h', '.hpp'))
    source_hits, encoding_bad = [], []
    for p in sources:
        try:
            raw = p.read_bytes()
            content = raw.decode('utf-8')
            if raw.startswith(b'\xef\xbb\xbf'):
                encoding_bad.append(str(p.relative_to(ROOT)))
        except UnicodeDecodeError:
            encoding_bad.append(str(p.relative_to(ROOT)))
            continue
        for number, line in enumerate(content.splitlines(), 1):
            if LOCKS.search(line):
                source_hits.append(f'{p.relative_to(ROOT)}:{number}:{line}')
    objects, linux_runtime = {}, {}

    def nm(path, undefined=False):
        p = subprocess.run([a.nm, *(['-u'] if undefined else []), str(path)],
                           capture_output=True, encoding='utf-8', errors='replace', check=True)
        (out / (path.name + '.nm.txt')).write_text(p.stdout + p.stderr, encoding='utf-8')
        return p.stdout

    for name in manifest['objects']:
        hits = [s.strip() for s in nm(build / name, True).splitlines() if SYMBOLS.search(s)]
        allowed = [s for s in hits if a.platform == 'linux' and ('__wait_impl' in s or '__notify_impl' in s)]
        objects[name] = [s for s in hits if s not in allowed]
        if allowed:
            linux_runtime[name] = allowed
    for name in manifest['allocator_objects']:
        if not (build / name).is_file():
            raise RuntimeError('Missing allocator object: ' + name)
    allocator_missing = []
    for name in manifest['executables']:
        symbols = nm(build / name)
        if not re.search(r'\b[Tt] _?mi_malloc$', symbols, re.M) or not re.search(r'\b[Tt] _?mi_version$', symbols, re.M):
            allocator_missing.append(name)
    passed = not (source_hits or encoding_bad or any(objects.values()) or allocator_missing)
    result = {'pass': passed, 'platform': a.platform, 'source_files': len(sources),
              'source_hits': source_hits, 'object_symbol_hits': objects,
              'allocator_objects_excluded': manifest['allocator_objects'],
              'allocator_missing_from_executables': allocator_missing,
              'linux_existing_atomic_wait': linux_runtime, 'encoding_bad': encoding_bad,
              'boundary': 'Product undefined symbols and source syntax; allocator and linked OS runtime internals are separate.'}
    (out / 'static_results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
