#!/usr/bin/env python3
"""Repository build matching the fixed Windows relay recipe; no downloads."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = {'vrs_cli.cpp', 'vrs_t_insert5.cpp', 'vrs_t_insert7.cpp',
           'vrs_t_conc.cpp', 'vrs_t_shut.cpp', 't6drv.cpp'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--platform', choices=('linux', 'windows'), required=True)
    ap.add_argument('--build')
    for name, default in (('cxx', 'g++'), ('cc', 'gcc'), ('ar', 'ar'), ('nm', 'nm')):
        ap.add_argument('--' + name, default=os.environ.get(name.upper(), default))
    args = ap.parse_args()
    win = args.platform == 'windows'
    if not win:
        os.sched_setaffinity(0, set(range(12)) & os.sched_getaffinity(0))
    build = Path(args.build or ('src/build-windows' if win else 'src/build'))
    if not build.is_absolute():
        build = ROOT / build
    build.mkdir(parents=True, exist_ok=True)
    manifest = build / 'build_manifest.json'
    manifest.unlink(missing_ok=True)
    log = (build / 'build.log').open('w', encoding='utf-8')

    def run(cmd):
        print(subprocess.list2cmdline(list(map(str, cmd))), flush=True)
        p = subprocess.run(list(map(str, cmd)), cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, encoding='utf-8', errors='replace')
        log.write(subprocess.list2cmdline(list(map(str, cmd))) + '\n' + p.stdout)
        log.flush()
        if p.stdout:
            print(p.stdout, end='', flush=True)
        p.check_returncode()

    lib = ROOT / 'native/libvrs'
    flags = ['-O2', '-std=c++20', '-pthread', '-ffp-contract=off', '-D_FILE_OFFSET_BITS=64']
    if win:
        flags += ['-D_WIN32_WINNT=0x0602', '-DWINVER=0x0602']
    flags += ['-I', lib / 'swegca_cpp', '-I', lib]
    units = sorted(p for p in lib.glob('*.cpp')
                   if (p.name not in DRIVERS and not p.name.startswith('vrs_plat_'))
                   or (win and p.name == 'vrs_plat_win.cpp'))
    units += sorted((lib / 'swegca_cpp/swegca_architecture').glob('*.cpp'))
    objects = []
    for source in units:
        obj = build / (source.stem + '.o')
        run([args.cxx, *flags, '-c', source, '-o', obj])
        objects.append(obj)
    archive = build / 'libvrs.a'
    archive.unlink(missing_ok=True)  # ar rcs otherwise retains removed members.
    run([args.ar, 'rcs', archive, *objects])
    mi = ROOT / 'third/mimalloc-2.1.7'
    run([args.cc, '-O2', '-DNDEBUG', '-I', mi / 'include', '-c', mi / 'src/static.c',
         '-o', build / 'mimalloc.o'])
    (build / 'mi_newdel.cpp').write_text('#include "mimalloc-new-delete.h"\n', encoding='utf-8')
    run([args.cxx, '-O2', '-std=c++20', '-I', mi / 'include', '-c', build / 'mi_newdel.cpp',
         '-o', build / 'mi_newdel.o'])
    allocators = [build / 'mi_newdel.o', build / 'mimalloc.o']
    tail = [*allocators, archive]
    tail += ['-lsynchronization', '-ladvapi32', '-lbcrypt', '-lpsapi', '-static'] if win else ['-ldl', '-lrt']
    drivers = {'vrs_cli': lib / 'vrs_cli.cpp', 'vrs_t_insert7': lib / 'vrs_t_insert7.cpp',
               'vrs_t_conc': lib / 'vrs_t_conc.cpp', 'reopen': ROOT / 'tools/reopen.cpp',
               'w5': ROOT / 'tools/w5.cpp', 'v603': ROOT / 'tools/v603/v603_portable.cpp'}
    exes = []
    suffix = '.exe' if win else ''
    for name, source in drivers.items():
        exe = build / (name + suffix)
        run([args.cxx, *flags, '-I', ROOT / 'src', source, *tail, '-o', exe])
        exes.append(exe)
    dobjs = []
    for source in sorted((ROOT / 'src').glob('*.cpp')):
        obj = build / ('d_' + source.stem + '.o')
        run([args.cxx, *flags, '-I', ROOT / 'src', '-c', source, '-o', obj])
        dobjs.append(obj)
    daemon = build / ('vrs_ccd' + suffix)
    run([args.cxx, *flags, *(['-municode'] if win else []), *dobjs, *tail, '-o', daemon])
    exes.append(daemon)
    unit = build / ('u_native' + suffix)
    run([args.cxx, *flags, '-I', ROOT / 'src', ROOT / 'src/tests/u_native.cpp',
         *[p for p in dobjs if p.name != 'd_vrs_ccd.o'], *tail, '-o', unit])
    exes.append(unit)
    manifest.write_text(json.dumps({'platform': args.platform, 'flags': list(map(str, flags)),
        'sources': [str(p.relative_to(ROOT)) for p in units],
        'objects': [p.name for p in objects + dobjs],
        'allocator_objects': [p.name for p in allocators], 'executables': [p.name for p in exes]},
        indent=2) + '\n', encoding='utf-8')
    run([sys.executable, '-B', ROOT / 'src/tests/static_check.py', '--build', build,
         '--platform', args.platform, '--nm', args.nm, '--output', build / 'audit'])
    log.close()


if __name__ == '__main__':
    main()
