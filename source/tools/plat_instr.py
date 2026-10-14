import re, sys
# inserts a timing guard at the start of selected vp_* functions in plat_prof.cpp (a copy of vrs_plat_win.cpp)
names = ['vp_path', 'vp_u8', 'vp_open_ro', 'vp_close', 'vp_pread', 'vp_fsize', 'vp_map_ro', 'vp_unmap',
         'vp_exists', 'vp_is_dir', 'vp_realpath', 'vp_tls_raw', 'vp_tls_set', 'vp_open_append', 'vp_append']
p = 'plat_prof.cpp'
s = open(p, encoding='utf-8').read()
head = '''
#include <atomic>
#include <cstdio>
struct ProfSlot { std::atomic<long long> ticks{0}, calls{0}; };
static ProfSlot g_prof[32];
static const char* g_prof_names[32];
struct ProfGuard { int i; LARGE_INTEGER t0; ProfGuard(int k) : i(k) { QueryPerformanceCounter(&t0); }
  ~ProfGuard() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); g_prof[i].ticks += t1.QuadPart - t0.QuadPart; g_prof[i].calls++; } };
extern "C" void vp_prof_dump() { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
  for (int i = 0; i < 32; i++) if (g_prof_names[i]) std::printf("%-16s calls %10lld  ms %10.1f\\n", g_prof_names[i], (long long)g_prof[i].calls, g_prof[i].ticks * 1000.0 / f.QuadPart); }
'''
# put the profiler after the last #include
last = [m.end() for m in re.finditer(r'#include[^\n]*\n', s)][-1]
s = s[:last] + head + s[last:]
init = []
for i, n in enumerate(names):
    m = re.search(r'^[^\n]*\b' + n + r'\([^\n]*?\)\s*\{', s, re.M)
    if not m:
        print('missing', n); continue
    s = s[:m.end()] + f' ProfGuard _pg({i});' + s[m.end():]
    init.append(f'g_prof_names[{i}] = "{n}";')
s += '\nstatic int g_prof_init = [] { ' + ' '.join(init) + ' return 0; }();\n'
open(p, 'w', encoding='utf-8').write(s)
print('ok', len(init))
