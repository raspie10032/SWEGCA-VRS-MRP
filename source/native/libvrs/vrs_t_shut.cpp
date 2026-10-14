// vrs_t_shut: T5-7. One process = one open/close of the library in the shape of a gate of the app (S1-S5), ending the way the app ends: std::exit(0) with the
// workers possibly still running (how=exit) or vrs_close first (how=close).
//   vrs_t_shut <shape> <how> <sessions_dir> <index_dir> <src rollout> <target file> <l0> <l1>
//   shapes: S1 (new session, ~23 lines, answer), S2 (the same session goes on: lines [l0,l1) more), S3 (a tool-heavy run: lines [l0,l1) fast, an answer every 25 lines),
//           S5 (reopen on the existing index, wait for the first answer, end), S5x (reopen and end at once, the restore is still running)
// The lines are handed over the way the rollout writer does: write, vrs_insert, vrs_written. Exit code: 0 ok, 3 no answer in 20 s, 4 open failed; a signal is the test's failure.
#include "vrs.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
static void put(int fd, const std::string& s) { size_t w = 0; while (w < s.size()) { ssize_t k = ::write(fd, s.data() + w, s.size() - w); if (k <= 0) return; w += (size_t)k; } }

int main(int argc, char** argv) {
  if (argc < 9) { std::fprintf(stderr, "usage: vrs_t_shut shape how sdir idir src target l0 l1\n"); return 2; }
  const std::string shape = argv[1], how = argv[2], sdir = argv[3], idir = argv[4], src = argv[5], target = argv[6];
  const size_t l0 = std::strtoul(argv[7], nullptr, 10);
  size_t l1 = std::strtoul(argv[8], nullptr, 10);
  vrs_opts o{}; o.sessions_dir = sdir.c_str(); o.index_dir = idir.c_str(); o.segwin = 1; o.budget = 258400; o.workers = 8;
  vrs_h* h = nullptr;
  if (vrs_open(&o, &h) != 0) return 4;
  auto finish = [&](int rc) { if (how == "close") vrs_close(h); std::exit(rc); };
  if (shape == "S5x") finish(0);
  auto answer = [&](const std::string& path, double secs) -> int {  // asks until rc 0 or the time is up
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      char* j = nullptr; size_t n = 0;
      const int rc = vrs_assemble(h, path.c_str(), &j, &n);
      vrs_free(j);
      if (rc == 0) return 0;
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > secs) return rc ? rc : 3;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };
  if (shape == "S5") {
    int rc = answer(target, 20.0);
    finish(rc == 0 ? 0 : 3);
  }
  std::vector<std::string> L; { std::ifstream in(src, std::ios::binary); std::string l; while (std::getline(in, l)) L.push_back(l); }
  if (l1 > L.size()) l1 = L.size();
  fs::create_directories(fs::path(target).parent_path());
  const int fd = ::open(target.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  off_t off = ::lseek(fd, 0, SEEK_END);
  for (size_t i = l0; i < l1; i++) {
    put(fd, L[i] + "\n");
    vrs_insert(h, target.c_str(), (uint64_t)off, L[i].data(), L[i].size());
    vrs_written(h, target.c_str(), (uint64_t)off);
    off += (off_t)L[i].size() + 1;
    if (shape == "S3" && (i - l0) % 25 == 24) { char* j = nullptr; size_t n = 0; vrs_assemble(h, target.c_str(), &j, &n); vrs_free(j); }
  }
  ::close(fd);
  const int rc = answer(target, 20.0);
  finish(rc == 0 ? 0 : 3);
  return 0;
}
