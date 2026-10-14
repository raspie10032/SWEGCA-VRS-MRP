// vrs_cli: test driver for libvrs.a
//   vrs_cli [--out DIR] [--budget N] [--workers N] [--cpus A-B] [--segwin 0|1] [--stats] [--repeat N] <sessions_dir> <index_dir|-> <rollout>...
// opens the library, waits until it is ready (prints the milliseconds from the start of main), assembles every rollout and writes the JSON
// of each to DIR/<file name>.json (or, without --out, prints one line per rollout).
#include "vrs.h"
#include "vrs_plat.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  const auto t0 = std::chrono::steady_clock::now();
  std::string out_dir; uint64_t budget = 258400; unsigned workers = 0; int segwin = 1; bool stats = false; int repeat = 1;
  std::vector<unsigned> cpus;
  int i = 1;
  for (; i < argc && std::strncmp(argv[i], "--", 2) == 0; i++) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == "--budget" && i + 1 < argc) budget = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--workers" && i + 1 < argc) workers = (unsigned)std::atoi(argv[++i]);
    else if (a == "--segwin" && i + 1 < argc) segwin = std::atoi(argv[++i]);
    else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
    else if (a == "--stats") stats = true;
    else if (a == "--cpus" && i + 1 < argc) {
      unsigned lo = 0, hi = 0;
      if (std::sscanf(argv[++i], "%u-%u", &lo, &hi) == 2) for (unsigned c = lo; c <= hi; c++) cpus.push_back(c);
    } else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  if (argc - i < 2) { std::fprintf(stderr, "usage: vrs_cli [options] <sessions_dir> <index_dir|-> <rollout>...\n"); return 2; }
  const std::string sdir = argv[i], idir = argv[i + 1];
  vrs_opts o{};
  o.sessions_dir = sdir.c_str(); o.index_dir = idir == "-" ? nullptr : idir.c_str(); o.workers = workers;
  o.cpus = cpus.empty() ? nullptr : cpus.data(); o.ncpus = (unsigned)cpus.size(); o.segwin = segwin; o.budget = budget;
  vrs_h* h = nullptr;
  int rc = vrs_open(&o, &h);
  if (rc != 0) { std::fprintf(stderr, "vrs_open failed: %d\n", rc); return 1; }
  const auto t_open = std::chrono::steady_clock::now();
  int rd;
  while ((rd = vrs_ready(h)) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto t1 = std::chrono::steady_clock::now();
  std::printf("ready=%d open_ms=%.1f ready_ms=%.1f\n", rd, std::chrono::duration<double, std::milli>(t_open - t0).count(), std::chrono::duration<double, std::milli>(t1 - t0).count());
  if (stats) {
    vrs_dbg d; vrs_dbg_stats(h, &d);
    if (d.dep_pairs) std::printf("dep_check pairs=%llu violations=%llu (candidates with theta_e(y) >= theta_e(x), reference turns with theta_e(r) >= theta_e(y))\n", (unsigned long long)d.dep_pairs, (unsigned long long)d.dep_viol);
    std::printf("stats items=%llu turns=%llu sessions=%llu lines_applied=%llu tail_lines=%llu restored_items=%llu cells_n=%llu cells_hash=%016llx theta_checked=%llu "
                "theta_mismatch=%llu theta_decreasing=%llu theta_maxgroup=%llu shards_loaded=%llu shards_rebuilt=%llu shard_corrupt=%llu shard_err=%llu lemma_e=%llu ready_us=%lld\n",
                (unsigned long long)d.items, (unsigned long long)d.turns, (unsigned long long)d.sessions, (unsigned long long)d.lines_applied, (unsigned long long)d.tail_lines,
                (unsigned long long)d.restored_items, (unsigned long long)d.cells_n, (unsigned long long)d.cells_hash, (unsigned long long)d.theta_checked,
                (unsigned long long)d.theta_mismatch, (unsigned long long)d.theta_decreasing, (unsigned long long)d.theta_maxgroup, (unsigned long long)d.shards_loaded,
                (unsigned long long)d.shards_rebuilt, (unsigned long long)d.shard_corrupt, (unsigned long long)d.shard_err, (unsigned long long)d.lemma_e, (long long)d.ready_us);
  }
  int bad = 0;
  std::vector<std::string> rolls;  // @file: one rollout per line, a relative line is under <sessions_dir> (the Windows command line is too short for 409 paths)
  for (int a = i + 2; a < argc; a++) {
    if (argv[a][0] != '@') { rolls.push_back(argv[a]); continue; }
    std::ifstream in(vp_io_path(argv[a] + 1), std::ios::binary);
    for (std::string l; std::getline(in, l);) {
      while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
      if (l.empty()) continue;
      rolls.push_back(l[0] == '/' || (l.size() > 1 && l[1] == ':') ? l : sdir + "/" + l);
    }
  }
  if (rd == 1) for (const std::string& roll : rolls) {
    for (int rep = 0; rep < repeat; rep++) {
      char* json = nullptr; size_t len = 0;
      const auto a0 = std::chrono::steady_clock::now();
      rc = vrs_assemble(h, roll.c_str(), &json, &len);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a0).count();
      if (rep + 1 < repeat) { vrs_free(json); continue; }
      std::printf("assemble rc=%d bytes=%zu us=%.0f %s\n", rc, len, us, roll.c_str());
      if (rc != 0) bad++;
      if (!out_dir.empty() && json) {
        const std::string p = roll;
        const size_t sl = p.rfind('/');
        const std::string fn = out_dir + "/" + (sl == std::string::npos ? p : p.substr(sl + 1)) + ".json";
        if (FILE* f = vp_fopen(fn.c_str(), "wb")) { std::fwrite(json, 1, len, f); std::fclose(f); }
      }
      vrs_free(json);
    }
  }
  vrs_close(h);
  return bad ? 1 : 0;
}
