// vrs_t_conc: W4 (VRS_MCP_libvrs_v1.md 14.2). The lines of three rollouts are put into an empty store the way a writer with positioned writes does (write the line
// at its offset, vrs_insert, vrs_written). ordered: one thread, file after file, line after line. shuffle: per file `threads` threads take the lines in a
// shuffled order (seed). Meanwhile one thread assembles the three files again and again. At the end (quiet store) the cells and the three answers are written.
//   vrs_t_conc ordered|shuffle <seed> <threads> <sessions_dir> <index_dir> <out file> <src1> <src2> <src3>
// out file: "cells_n=.. cells_hash=.. items=.. turns=.. lines=..", then per file "== <name>" and the answer without its "us" field. Exit code: 0, or 3 when an
// assembly during the run returned 2 or a negative value (counted in the out file as "bad_rc=").
#include "vrs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
struct Line { uint64_t off; std::string s; };
static std::vector<Line> read_lines(const std::string& p) {
  std::ifstream in(fs::path(p), std::ios::binary);
  std::vector<Line> v; std::string l; uint64_t off = 0;
  while (std::getline(in, l)) { v.push_back({off, l}); off += l.size() + 1; }
  return v;
}
struct WFile {  // one positioned writer per thread
#ifdef _WIN32
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit WFile(const std::string& p) {
    h = CreateFileA(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  void put(uint64_t off, const std::string& s) {
    OVERLAPPED ov{}; ov.Offset = (DWORD)(off & 0xffffffffu); ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD w = 0; WriteFile(h, s.data(), (DWORD)s.size(), &w, &ov);
  }
  ~WFile() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
#else
  int fd = -1;
  explicit WFile(const std::string& p) { fd = ::open(p.c_str(), O_WRONLY | O_CREAT, 0644); }
  void put(uint64_t off, const std::string& s) { size_t w = 0; while (w < s.size()) { ssize_t k = ::pwrite(fd, s.data() + w, s.size() - w, (off_t)(off + w)); if (k <= 0) break; w += (size_t)k; } }
  ~WFile() { if (fd >= 0) ::close(fd); }
#endif
};

int main(int argc, char** argv) {
  if (argc < 10) { std::fprintf(stderr, "usage: vrs_t_conc ordered|shuffle <seed> <threads> <sessions_dir> <index_dir> <out> <src1> <src2> <src3>\n"); return 2; }
  const std::string mode = argv[1]; const unsigned seed = (unsigned)std::atoi(argv[2]); const int nth = std::max(1, std::atoi(argv[3]));
  const std::string sdir = argv[4], idir = argv[5], outp = argv[6];
  std::vector<std::string> src = {argv[7], argv[8], argv[9]}, dst;
  std::vector<std::vector<Line>> L;
  fs::remove_all(fs::path(sdir)); fs::remove_all(fs::path(idir));
  fs::create_directories(fs::path(sdir + "/2026/01/01")); fs::create_directories(fs::path(idir));
  for (auto& s : src) { L.push_back(read_lines(s)); dst.push_back(sdir + "/2026/01/01/" + fs::path(s).filename().string()); { std::ofstream o(fs::path(dst.back()), std::ios::binary); } }
  vrs_opts o{}; o.sessions_dir = sdir.c_str(); o.index_dir = idir.c_str(); o.segwin = 1; o.budget = 258400; o.workers = 12;
  vrs_h* h = nullptr;
  if (vrs_open(&o, &h) != 0) return 4;
  while (vrs_ready(h) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::atomic<int> done{0}; std::atomic<uint64_t> asm_n{0}, bad_rc{0};
  std::thread asm_t([&] {
    while (!done.load()) for (auto& d : dst) {
      char* j = nullptr; size_t n = 0; const int rc = vrs_assemble(h, d.c_str(), &j, &n); vrs_free(j);
      asm_n++; if (rc == 2 || rc < 0) bad_rc++;
    }
  });
  uint64_t total = 0; for (auto& v : L) total += v.size();
  auto hand = [&](size_t f, const std::vector<size_t>& idx) {
    WFile w(dst[f]);
    for (size_t i : idx) {
      const Line& l = L[f][i];
      w.put(l.off, l.s + "\n");
      vrs_insert(h, dst[f].c_str(), l.off, l.s.data(), l.s.size());
      vrs_written(h, dst[f].c_str(), l.off);
    }
  };
  if (mode == "ordered") {
    for (size_t f = 0; f < 3; f++) { std::vector<size_t> idx(L[f].size()); for (size_t i = 0; i < idx.size(); i++) idx[i] = i; hand(f, idx); }
  } else {
    std::vector<std::thread> ts;
    for (size_t f = 0; f < 3; f++) {
      std::vector<size_t> idx(L[f].size()); for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
      std::mt19937_64 rng(seed * 1000003ull + f); std::shuffle(idx.begin(), idx.end(), rng);
      for (int t = 0; t < nth; t++) {
        std::vector<size_t> part; for (size_t k = (size_t)t; k < idx.size(); k += (size_t)nth) part.push_back(idx[k]);
        ts.emplace_back(hand, f, part);
      }
    }
    for (auto& t : ts) t.join();
  }
  vrs_dbg d{};
  for (int i = 0; i < 60000; i++) { vrs_dbg_stats(h, &d); if (d.lines_applied >= total && d.pend == 0) break; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  done.store(1); asm_t.join();
  vrs_dbg_stats(h, &d);
  std::string out = "cells_n=" + std::to_string(d.cells_n) + " cells_hash=" + std::to_string(d.cells_hash) + " items=" + std::to_string(d.items) + " turns=" + std::to_string(d.turns) +
                    " lines=" + std::to_string(d.lines_applied) + "\n";
  for (auto& dd : dst) {
    char* j = nullptr; size_t n = 0; const int rc = vrs_assemble(h, dd.c_str(), &j, &n);
    std::string a = j ? std::string(j, n) : std::string(); vrs_free(j);
    const size_t u = a.rfind(",\"us\":"); if (u != std::string::npos) a.resize(u);
    out += "== " + fs::path(dd).filename().string() + " rc=" + std::to_string(rc) + "\n" + a + "\n";
  }
  vrs_close(h);
  { std::ofstream f(fs::path(outp), std::ios::binary); f << out; }
  std::printf("%s seed=%u threads=%d lines=%llu/%llu assemblies=%llu bad_rc=%llu\n", mode.c_str(), seed, nth, (unsigned long long)d.lines_applied, (unsigned long long)total,
              (unsigned long long)asm_n.load(), (unsigned long long)bad_rc.load());
  return bad_rc.load() ? 3 : 0;
}
