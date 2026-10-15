// T3-1 driver (VRS_K5_query_v1.md §15): open lib7 + K5 on fixed roots, wait until the K5 layer is ready and at rest, write the dump of
// K5R_DUMP_SPEC.md. usage: k5drv --root TAG DIR [--root TAG DIR] --idx DIR [--workers N] [--queries FILE]... --out FILE [--wait-s N]
// The query files are the calculator's: a list [{"id","q"}] or {"items":[{"id"?, "query"}]} (id = stem-NN when absent).
#include "vrs.h"
#include "cc_json.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::string slurp(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss; ss << in.rdbuf(); return ss.str();
}
static std::string stem_of(const std::string& p) {
  size_t a = p.find_last_of("/\\"); a = a == std::string::npos ? 0 : a + 1;
  size_t b = p.rfind('.'); if (b == std::string::npos || b < a) b = p.size();
  return p.substr(a, b - a);
}

int main(int argc, char** argv) {
  std::vector<std::string> tags, dirs, qfiles;
  std::string idx, out; unsigned workers = 0; int wait_s = 3600, hold_s = 0;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--root" && i + 2 < argc) { tags.push_back(argv[i + 1]); dirs.push_back(argv[i + 2]); i += 2; }
    else if (a == "--idx" && i + 1 < argc) idx = argv[++i];
    else if (a == "--workers" && i + 1 < argc) workers = unsigned(std::stoul(argv[++i]));
    else if (a == "--queries" && i + 1 < argc) qfiles.push_back(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--wait-s" && i + 1 < argc) wait_s = std::stoi(argv[++i]);
    else if (a == "--hold-s" && i + 1 < argc) hold_s = std::stoi(argv[++i]);  // T3-1d: stay up after the dump (the harness kills it)
    else { std::fprintf(stderr, "k5drv: bad argument %s\n", a.c_str()); return 2; }
  }
  if (dirs.empty() || out.empty()) { std::fprintf(stderr, "k5drv: --root and --out are required\n"); return 2; }
  std::vector<std::string> ids, qs;
  for (const auto& f : qfiles) {
    const cc::Json j = cc::parse(slurp(f));
    if (j.kind == cc::Json::array) for (const auto& it : j.values) { ids.push_back(it.at("id").text()); qs.push_back(it.at("q").text()); }
    else {
      size_t n = 0;
      for (const auto& it : j.at("items").values) {
        n++;
        char b[32]; std::snprintf(b, sizeof b, "%s-%02zu", stem_of(f).c_str(), n);
        ids.push_back(it.find("id") ? it.at("id").text() : std::string(b)); qs.push_back(it.at("query").text());
      }
    }
  }
  std::vector<vrs_root> roots(dirs.size());
  for (size_t i = 0; i < dirs.size(); i++) { roots[i] = vrs_root{}; roots[i].tag = tags[i].c_str(); roots[i].dir = dirs[i].c_str(); }
  vrs_opts o{}; o.index_dir = idx.c_str(); o.workers = workers; o.roots = roots.data(); o.nroots = unsigned(roots.size());
  const auto t0 = std::chrono::steady_clock::now();
  vrs_h* h = nullptr;
  int rc = vrs_open(&o, &h);
  if (rc) { std::fprintf(stderr, "k5drv: vrs_open %d\n", rc); return 2; }
  auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  double t_lib = -1, t_k5 = -1;
  for (;;) {
    const int r = vrs_ready(h);
    if (r < 0) { std::fprintf(stderr, "k5drv: vrs_ready %d\n", r); return 2; }
    if (r == 1 && t_lib < 0) { t_lib = secs(); std::fprintf(stderr, "K5DRV lib_ready %.3f\n", t_lib); }
    vrs_k5dbg d{}; vrs_k5_stats(h, &d);
    if (vrs_k5_ready(h) == 1 && t_k5 < 0) { t_k5 = secs(); std::fprintf(stderr, "K5DRV k5_ready %.3f\n", t_k5); }
    if (t_k5 >= 0 && d.pend <= 0) break;
    if (d.shadow_bad) { std::fprintf(stderr, "K5DRV shadow failure\n"); return 3; }
    if (secs() > wait_s) { std::fprintf(stderr, "K5DRV timeout pend=%lld base=%lld\n", (long long)d.pend, (long long)d.base); return 4; }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::vector<const char*> pi, pq;
  for (size_t i = 0; i < ids.size(); i++) { pi.push_back(ids[i].c_str()); pq.push_back(qs[i].c_str()); }
  const double t_d0 = secs();
  rc = vrs_k5_dump(h, pi.data(), pq.data(), unsigned(ids.size()), out.c_str());
  const double t_dump = secs() - t_d0;
  const int cells_bad = vrs_k5_check_cells(h);
  vrs_k5dbg d{}; vrs_k5_stats(h, &d);
  vrs_dbg g{}; vrs_dbg_stats(h, &g);
  std::printf("{\"rc\":%d,\"lib_ready_s\":%.3f,\"k5_ready_s\":%.3f,\"dump_s\":%.3f,\"turns\":%llu,\"claimed\":%llu,\"units_built\":%llu,\"units_restored\":%llu,"
              "\"units_checked\":%llu,\"revisits\":%llu,\"rev_tasks\":%llu,\"shadow_n\":%llu,\"shadow_bad\":%llu,\"cells_bad\":%d,\"size\":%llu,\"base_entries\":%llu,"
              "\"node_bytes\":%llu,\"unit_bytes\":%llu,\"retries\":%llu,\"shards_loaded\":%llu,\"shards_rebuilt\":%llu,\"syn_bytes\":%llu,\"syn_miss\":%llu,"
              "\"ex_total\":%llu,\"ex_bad\":%llu}\n",
              rc, t_lib, t_k5, t_dump, (unsigned long long)g.turns, (unsigned long long)d.claimed, (unsigned long long)d.units_built, (unsigned long long)d.units_restored,
              (unsigned long long)d.units_checked, (unsigned long long)d.revisits, (unsigned long long)d.rev_tasks, (unsigned long long)d.shadow_n,
              (unsigned long long)d.shadow_bad, cells_bad, (unsigned long long)d.size, (unsigned long long)d.base_entries, (unsigned long long)d.node_bytes,
              (unsigned long long)d.unit_bytes, (unsigned long long)d.retries, (unsigned long long)g.shards_loaded, (unsigned long long)g.shards_rebuilt,
              (unsigned long long)d.syn_bytes, (unsigned long long)d.syn_miss, (unsigned long long)d.ex_total, (unsigned long long)d.ex_bad);
  std::fflush(stdout);
  if (hold_s) std::this_thread::sleep_for(std::chrono::seconds(hold_s));
  vrs_close(h);
  return rc ? 5 : (d.shadow_bad || cells_bad) ? 3 : 0;
}
