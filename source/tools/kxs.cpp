// usage: kxs <src root> <list (rel paths, in feed order)> <work dir> -- streams each file line by line (append, vrs_insert, vrs_written), one thread
#include "vrs.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;
static uint64_t dir_bytes(const fs::path& p) { uint64_t s = 0; for (auto& e : fs::recursive_directory_iterator(p)) if (e.is_regular_file()) s += e.file_size(); return s; }
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string src = argv[1], work = argv[3], sd = work + "/s", id = work + "/i";
  std::vector<std::string> rels; { std::ifstream l(argv[2]); for (std::string s; std::getline(l, s);) if (!s.empty()) rels.push_back(s); }
  fs::create_directories(sd); fs::create_directories(id);
  vrs_opts o{}; o.sessions_dir = sd.c_str(); o.index_dir = id.c_str(); o.workers = 6;
  vrs_h* h = nullptr; if (vrs_open(&o, &h)) return 3;
  while (vrs_ready(h) == 0) {}
  size_t n = 0;
  for (auto& r : rels) {
    const std::string dst = sd + "/" + r; fs::create_directories(fs::path(dst).parent_path());
    std::ifstream in(src + "/" + r, std::ios::binary); std::ofstream out(dst, std::ios::binary | std::ios::app);
    uint64_t off = 0;
    for (std::string line; std::getline(in, line);) {
      if (in.eof()) break;
      out << line << '\n'; out.flush();
      vrs_insert(h, dst.c_str(), off, line.data(), line.size()); vrs_written(h, dst.c_str(), off);
      off += line.size() + 1;
    }
    if (++n % 100 == 0 || n == rels.size()) {
      vrs_dbg st{}; vrs_dbg_stats(h, &st);
      std::printf("files %zu idx %llu late %llu recounts %llu x_total %llu\n", n, (unsigned long long)dir_bytes(id), (unsigned long long)st.late_events, (unsigned long long)st.recounts, (unsigned long long)st.x_total); std::fflush(stdout);
    }
  }
  vrs_close(h);
}
