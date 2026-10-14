// usage: reopen <sessions_dir> <index_dir> [workers=6]
// Native B diagnostic: requested workers, then one worker, identical ready condition.
#include "vrs.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "../third/mimalloc-2.1.7/include/mimalloc.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
static void memory_stats(const char* phase) {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX p{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p), sizeof p)) {
    std::printf("memory phase=%s error=%lu\n", phase, (unsigned long)GetLastError()); return;
  }
  std::printf("memory phase=%s page_faults=%lu peak_rss_bytes=%llu rss_bytes=%llu private_bytes=%llu\n", phase,
    (unsigned long)p.PageFaultCount, (unsigned long long)p.PeakWorkingSetSize,
    (unsigned long long)p.WorkingSetSize, (unsigned long long)p.PrivateUsage);
#else
  rusage r{};
  if (getrusage(RUSAGE_SELF, &r)) { std::printf("memory phase=%s error=getrusage\n", phase); return; }
  std::printf("memory phase=%s page_faults=%llu minor_faults=%llu major_faults=%llu peak_rss_bytes=%llu\n", phase,
    (unsigned long long)(r.ru_minflt + r.ru_majflt), (unsigned long long)r.ru_minflt,
    (unsigned long long)r.ru_majflt, (unsigned long long)r.ru_maxrss * 1024);
#endif
}
struct Usage { double user_ms, kernel_ms; unsigned long long faults; };
static Usage usage() {
#ifdef _WIN32
  FILETIME c{}, e{}, k{}, u{};
  GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
  auto ms = [](FILETIME t) { return ((double)t.dwHighDateTime * 4294967296.0 + t.dwLowDateTime) / 10000.0; };
  PROCESS_MEMORY_COUNTERS m{};
  GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m);
  return {ms(u), ms(k), m.PageFaultCount};
#else
  rusage r{}; getrusage(RUSAGE_SELF, &r);
  auto ms = [](timeval t) { return t.tv_sec * 1000.0 + t.tv_usec / 1000.0; };
  return {ms(r.ru_utime), ms(r.ru_stime), (unsigned long long)(r.ru_minflt + r.ru_majflt)};
#endif
}
static int run(const char* sessions, const char* index, unsigned workers) {
  std::printf("memory_run workers=%u\n", workers);
  memory_stats("before_open");
  const Usage u0 = usage();
  const auto t0 = std::chrono::steady_clock::now();
  vrs_opts o{}; o.sessions_dir = sessions; o.index_dir = index; o.workers = workers;
  vrs_h* h = nullptr;
  const int opened = vrs_open(&o, &h);
  if (opened) { std::fprintf(stderr, "open_rc %d workers %u\n", opened, workers); return 3; }
  const int waited = vrs_wait(h, nullptr, 120000);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const Usage u1 = usage();
  if (waited || vrs_ready(h) != 1) {
    std::fprintf(stderr, "wait_rc %d workers %u\n", waited, workers);
    vrs_close(h); return 4;
  }
  memory_stats("ready");
  vrs_dbg d{}; vrs_dbg_stats(h, &d);
  std::printf("ready_ms %.1f workers %u user_ms %.1f kernel_ms %.1f faults %llu ready_us %lld shards_loaded %llu shards_rebuilt %llu sessions %llu lines_applied %llu tail_lines %llu restored_items %llu recounts %llu insert_waits %llu pass_retries %llu count_retries %llu tasks %llu cells_hash %016llx\n", ms, workers,
    u1.user_ms-u0.user_ms, u1.kernel_ms-u0.kernel_ms, u1.faults-u0.faults, (long long)d.ready_us,
    (unsigned long long)d.shards_loaded, (unsigned long long)d.shards_rebuilt, (unsigned long long)d.sessions,
    (unsigned long long)d.lines_applied, (unsigned long long)d.tail_lines, (unsigned long long)d.restored_items,
    (unsigned long long)d.recounts, (unsigned long long)d.insert_waits, (unsigned long long)d.pass_retries,
    (unsigned long long)d.count_retries, (unsigned long long)d.tasks, (unsigned long long)d.cells_hash);
  vrs_close(h);
  memory_stats("closed");
  return 0;
}
int main(int argc, char** argv) {
  if (argc < 3) return 2;
#ifdef _WIN32
  if (!SetProcessAffinityMask(GetCurrentProcess(), 0xfff)) return 5;
#endif
  const unsigned workers = argc > 3 ? (unsigned)std::atoi(argv[3]) : 6;
  if (!workers || workers > 12) return 2;
  const int rc = run(argv[1], argv[2], workers);
  const int result = rc || workers == 1 ? rc : run(argv[1], argv[2], 1);
  std::printf("allocator mimalloc_version=%d\n", mi_version());
  mi_stats_merge();
  mi_stats_print(nullptr);
  return result;
}
