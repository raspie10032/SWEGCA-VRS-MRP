// Round 6 tests T6-1 (alias) and T6-2 (wait). VRS_코덱스일체화_v1.md §8.4.
// usage:
//   t6drv alias <sdir as opened> <idir> <src rollout> <new file, spelled one way> <same file, spelled another way> <n lines>
//     inserts n lines of src into the new file through spelling A, then assembles through A and B;
//     prints sessions before/after, shards_rebuilt, both byte counts and whether the answers are equal.
//   t6drv wait <sdir> <idir> <rollout to assemble> <out file>
//     opens, assembles at once (expect rc 1 under the hold), vrs_wait(path, 20000), assembles again; writes the items to <out file>.
#include "vrs.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static vrs_h* open_store(const char* sd, const char* id) {
  static std::vector<unsigned> cpus;
  for (unsigned c = 0; c < 12; c++) cpus.push_back(c);
  vrs_opts o{}; o.sessions_dir = sd; o.index_dir = id; o.workers = 12; o.cpus = cpus.data(); o.ncpus = 12; o.segwin = 1; o.budget = 0;
  vrs_h* h = nullptr;
  if (vrs_open(&o, &h) != 0) { std::fprintf(stderr, "open failed\n"); std::exit(1); }
  return h;
}
static std::string items_of(const char* j, size_t n) {  // the items array: the answer without its timing field
  std::string s(j, n);
  const size_t a = s.find("\"items\":"), b = s.rfind("],\"tokens\"");
  return (a == std::string::npos || b == std::string::npos) ? s : s.substr(a, b - a + 1);
}

int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage\n"); return 2; }
  const auto t0 = std::chrono::steady_clock::now();
  auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
  const std::string mode = argv[1];
  vrs_h* h = open_store(argv[2], argv[3]);
  if (mode == "alias") {
    if (argc < 8) return 2;
    while (vrs_ready(h) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    vrs_dbg d0; vrs_dbg_stats(h, &d0);
    const char* A = argv[5]; const char* B = argv[6]; const int n = std::atoi(argv[7]);
    std::ifstream in(argv[4]); std::string ln; std::vector<std::string> lines;
    while ((int)lines.size() < n && std::getline(in, ln)) lines.push_back(ln);
    if (std::getenv("T6_LATE_DIR") && !lines.empty()) {  // the app hands the first line in before the day directory exists
      if (vrs_insert(h, A, 0, lines[0].data(), lines[0].size()) < 0) std::printf("early insert failed\n");
      const std::string a(A);
      std::filesystem::create_directories(a.substr(0, a.rfind('/')));
    }
    const int fd = ::open(A, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { std::perror("create"); return 1; }
    uint64_t off = 0;
    for (size_t i = 0; i < lines.size(); i++) {
      std::string b = lines[i] + "\n";
      if (::pwrite(fd, b.data(), b.size(), (off_t)off) != (ssize_t)b.size()) return 1;
      const char* via = (i % 2) ? B : A;  // the two spellings, alternately: one file must stay one session
      if (vrs_insert(h, via, off, lines[i].data(), lines[i].size()) < 0) std::printf("insert failed at %llu\n", (unsigned long long)off);
      vrs_written(h, via, off);
      off += b.size();
    }
    ::close(fd);
    char* ja = nullptr; size_t la = 0; char* jb = nullptr; size_t lb = 0;
    int ra = 1, rb = 1;
    for (int k = 0; k < 500 && (ra == 1 || rb == 1); k++) {
      if (ja) { vrs_free(ja); ja = nullptr; } if (jb) { vrs_free(jb); jb = nullptr; }
      ra = vrs_assemble(h, A, &ja, &la); rb = vrs_assemble(h, B, &jb, &lb);
      if (ra == 1 || rb == 1) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    vrs_dbg d1; vrs_dbg_stats(h, &d1);
    const bool same = ja && jb && items_of(ja, la) == items_of(jb, lb);
    std::printf("alias sessions_before %llu sessions_after %llu shards_loaded %llu shards_rebuilt %llu rcA %d rcB %d bytesA %zu bytesB %zu same %d\n",
                (unsigned long long)d0.sessions, (unsigned long long)d1.sessions, (unsigned long long)d1.shards_loaded, (unsigned long long)d1.shards_rebuilt,
                ra, rb, la, lb, same ? 1 : 0);
    if (ja) vrs_free(ja); if (jb) vrs_free(jb);
    vrs_close(h);
    return (same && ra == 0 && rb == 0 && d1.sessions == d0.sessions + 1) ? 0 : 3;
  }
  if (mode == "wait") {
    const char* P = argv[4];
    char* j = nullptr; size_t len = 0;
    const int r0 = vrs_assemble(h, P, &j, &len);
    if (j) { vrs_free(j); j = nullptr; }
    std::printf("%.0f ms first assemble rc=%d ready=%d\n", ms(), r0, vrs_ready(h));
    int w = 1, r1 = 1, rounds = 0;
    while (r1 == 1 && rounds < 60) {
      w = vrs_wait(h, P, 20000);
      std::printf("%.0f ms wait rc=%d ready=%d\n", ms(), w, vrs_ready(h));
      if (w < 0) break;
      r1 = vrs_assemble(h, P, &j, &len);
      rounds++;
    }
    std::printf("%.0f ms assemble rc=%d bytes=%zu rounds=%d\n", ms(), r1, len, rounds);
    if (j && r1 == 0) { const std::string it = items_of(j, len); std::ofstream o(argv[5], std::ios::binary); o << it; }
    if (j) vrs_free(j);
    vrs_close(h);
    return w < 0 ? 4 : (r1 == 0 ? 0 : 3);
  }
  return 2;
}
