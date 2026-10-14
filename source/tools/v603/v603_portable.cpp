// Portable companion: original v603.cpp remains byte-identical. Win32 positioned IO and LLP64-safe ordinal types only.
// V6-03 reproducer: replay the operations of a failed T-a3 (fork of a fork) in a fresh store, the Hermes way
// (pwrite the line, then vrs_insert, then vrs_written), and check each assembly holds every user text of its file's view.
// usage: v603 <ops.txt> <work dir> <rounds>
#include "vrs.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include "cc_io.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
namespace fs = std::filesystem;

struct Op { char k; int f; std::string line; };

// the user texts a correct assembly of file i must hold: the user texts of the lineage view (computed from the ops: root lines below the
// child's cut, own lines), taken from the lines written so far
static std::vector<std::string> expect(const std::vector<Op>& ops, size_t upto, int fi, const std::vector<std::pair<int, int64_t>>& cutof) {
  // cutof[i] = (parent index, cut ordinal) of file i, parent -1 for the root
  std::vector<std::vector<std::pair<int64_t, std::string>>> per(cutof.size());
  for (size_t q = 0; q < upto; q++) {
    const Op& o = ops[q];
    if (o.k != 'W') continue;
    const auto po = o.line.find("\"ordinal\":");
    const int64_t ord = std::strtoll(o.line.c_str() + po + 10, nullptr, 10);
    const auto pu = o.line.find("\"role\":\"user\",");
    if (pu == std::string::npos) continue;
    const auto pt = o.line.find("\"text\":\"", pu);
    const auto pe = o.line.find('"', pt + 8);
    per[o.f].push_back({ord, o.line.substr(pt + 8, pe - pt - 8)});
  }
  std::vector<std::string> out;
  std::vector<int> chain;
  for (int i = fi; i >= 0; i = cutof[i].first) chain.insert(chain.begin(), i);
  for (size_t c = 0; c < chain.size(); c++) {
    int64_t lim = int64_t(1) << 40;
    for (size_t d = c + 1; d < chain.size(); d++) lim = std::min(lim, cutof[chain[d]].second);
    for (auto& [o, t] : per[chain[c]]) if (o < lim) out.push_back(t);
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  std::vector<Op> ops;
  { std::ifstream in(argv[1]); std::string l;
    while (std::getline(in, l)) { Op o; o.k = l[0]; o.f = std::atoi(l.c_str() + 2); const auto sp = l.find(' ', 2); if (o.k == 'W') o.line = l.substr(sp + 1); ops.push_back(o); } }
  int nf = 0; for (auto& o : ops) nf = std::max(nf, o.f + 1);
  std::vector<std::pair<int, int64_t>> cutof(nf, {-1, 0});
  for (auto& o : ops) if (o.k == 'W' && o.line.find("\"forked_from_ordinal_exclusive\":") != std::string::npos) {
    const auto p = o.line.find("\"forked_from_ordinal_exclusive\":");
    cutof[o.f] = {o.f - 1, std::strtoll(o.line.c_str() + p + 32, nullptr, 10)};
  }
  const int rounds = std::atoi(argv[3]);
  int bad = 0;
  std::vector<std::string> failures;  // bounded diagnostics; never changes the first assembly's verdict
  std::vector<unsigned> cpus; for (unsigned c = 0; c < 12; c++) cpus.push_back(c);
  for (int r = 0; r < rounds; r++) {
    const std::string W = std::string(argv[2]) + "/r" + std::to_string(r);
    fs::remove_all(W); fs::create_directories(W + "/s/h"); fs::create_directories(W + "/i");
    vrs_opts o{}; const std::string sd = W + "/s", id = W + "/i";
    o.sessions_dir = sd.c_str(); o.index_dir = id.c_str(); o.workers = 12; o.cpus = cpus.data(); o.ncpus = 12; o.segwin = 1; o.budget = 0;
    vrs_h* h = nullptr;
    if (vrs_open(&o, &h)) { std::printf("open failed\n"); return 1; }
    while (vrs_ready(h) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::vector<std::string> paths(nf); std::vector<uint64_t> offs(nf, 0);
    { std::ifstream nm(std::string(argv[1]).substr(0, std::string(argv[1]).rfind('/')) + "/names.txt"); std::string l; int i = 0;
      while (i < nf && std::getline(nm, l)) paths[i++] = W + "/s/h/" + l; }  // the captured names: the lineage refers to their ids
    for (size_t q = 0; q < ops.size(); q++) {
      const Op& op = ops[q];
      if (op.k == 'W') {
        const std::string b = op.line + "\n";
#ifdef _WIN32
        { cc::File file(paths[op.f], true); file.write_once(b, offs[op.f]); }
#else
        const int fd = ::open(paths[op.f].c_str(), O_WRONLY | O_CREAT, 0644);
        if (::pwrite(fd, b.data(), b.size(), (off_t)offs[op.f]) != (ssize_t)b.size()) return 1;
        ::close(fd);
#endif
        vrs_insert(h, paths[op.f].c_str(), offs[op.f], op.line.data(), op.line.size());
        vrs_written(h, paths[op.f].c_str(), offs[op.f]);
        offs[op.f] += b.size();
      } else {
        char* j = nullptr; size_t len = 0;
        int rc = vrs_assemble(h, paths[op.f].c_str(), &j, &len);
        for (int t = 0; rc == 1 && t < 2000; t++) { if (j) { vrs_free(j); j = nullptr; } vrs_wait(h, paths[op.f].c_str(), 1000); rc = vrs_assemble(h, paths[op.f].c_str(), &j, &len); }
        const std::string got = j ? std::string(j, len) : std::string();
        bool ok = rc == 0;
        const auto ex = expect(ops, q, op.f, cutof);
        size_t pos = 0; std::string missing;
        for (auto& t : ex) { const auto p = got.find("\"text\":\"" + t + "\"", pos); if (p == std::string::npos) { ok = false; missing = t; break; } pos = p; }
        if (!ok) {
          bad++;
          vrs_visibility visibility{}; vrs_dbg_visibility(&visibility);
          vrs_dbg diagnostic{}; vrs_dbg_stats(h, &diagnostic);
          std::printf("round %d op %zu file %d rc %d MISSING; expected:", r, q, op.f, rc);
          for (auto& t : ex) std::printf(" [%s]", t.c_str());
          std::printf("\n got: %s\n", got.substr(0, 1500).c_str());
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          char* j2 = nullptr; size_t l2 = 0; const int rc2 = vrs_assemble(h, paths[op.f].c_str(), &j2, &l2);
          std::printf(" again after 300 ms rc %d: %s\n", rc2, j2 ? std::string(j2, l2).substr(0, 1500).c_str() : "");
          const std::string later = j2 ? std::string(j2, l2) : std::string();
          bool later_ok = rc2 == 0; size_t later_pos = 0;
          for (const auto& t : ex) { const auto p = later.find("\"text\":\"" + t + "\"", later_pos); if (p == std::string::npos) { later_ok = false; break; } later_pos = p; }
          if (failures.size() < 3) {
            char detail[256];
            std::snprintf(detail, sizeof detail, "r%d/op%zu/f%d rc%d missing=%.16s pend=%llu lines=%llu/%llu later=%d pub=%llu front=%llu end=%llu off=%llu dup=%u sg=%u/%u",
                r, q, op.f, rc, missing.c_str(), (unsigned long long)diagnostic.pend,
                (unsigned long long)diagnostic.lines_applied, (unsigned long long)diagnostic.lines_in, int(later_ok),
                (unsigned long long)visibility.pub, (unsigned long long)visibility.front,
                (unsigned long long)visibility.end, (unsigned long long)visibility.off,
                visibility.duplicate, visibility.insert_sg, visibility.view_sg);
            failures.emplace_back(detail);
          }
          if (j2) vrs_free(j2);
        }
        if (j) vrs_free(j);
      }
    }
    vrs_close(h);
    fs::remove_all(W);
  }
  for (const auto& failure : failures) std::printf("W6_FAIL %s\n", failure.c_str());
  std::printf("rounds %d bad %d\n", rounds, bad);
  return bad ? 3 : 0;
}
