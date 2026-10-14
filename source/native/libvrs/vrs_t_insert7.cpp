// W3 (VRS_MCP_libvrs_v1.md 14.2): vrs_t_insert5.cpp for lib7 on both platforms: append without POSIX calls, the lib7 record magic.
// vrs_t_insert: tests of the insert / restore / replay paths of libvrs (E3-1 (b) and the paths the static comparison does not reach).
//   vrs_t_insert idem   <source rollout> <work dir>      same line twice leaves table and shard identical; restore == insert
//   vrs_t_insert replay <source rollout> <work dir>      shard written for a prefix of the file, the rest appended, restart: == a full parse
//   vrs_t_insert ooo    <source rollout> <work dir>      lines inserted in reverse order (gaps) end in the same tables as in order
// Every check prints PASS or FAIL; the exit code is the number of failures.
#include "vrs.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
static int g_fail = 0;
static void check(bool ok, const std::string& what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) g_fail++; }

struct Line { uint64_t off; std::string s; };
static std::vector<Line> read_lines(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  std::vector<Line> v; std::string l; uint64_t off = 0;
  while (std::getline(in, l)) { v.push_back({off, l}); off += l.size() + 1; }
  return v;
}
static std::string slurp(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void write_file(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream o(p, std::ios::binary | std::ios::trunc); o << s;
}
static void append_file(const std::string& p, const std::string& s) {
  std::ofstream o(fs::path(p), std::ios::binary | std::ios::app); o << s;  // W3 port: the platform has no O_APPEND; one process writes, so a stream append is the same
}
static std::string prefix_text(const std::vector<Line>& L, size_t n) { std::string s; for (size_t i = 0; i < n; i++) { s += L[i].s; s += '\n'; } return s; }

struct Frame { uint8_t type; std::string pl; };
static std::vector<Frame> read_frames_buf(const std::string& b) {
  std::vector<Frame> out;
  size_t pos = 0;
  while (pos + 16 <= b.size()) {
    uint32_t magic, len; std::memcpy(&magic, b.data() + pos, 4); std::memcpy(&len, b.data() + pos + 8, 4);
    if (magic != 0x36305856u ||  /* W3: the record magic of lib7 (VX06); this tool was written for lib5 (VX05) */ pos + 16 + len > b.size()) break;
    out.push_back({(uint8_t)b[pos + 4], b.substr(pos + 16, len)});
    pos += 16 + len;
  }
  return out;
}
static std::vector<Frame> read_frames(const std::string& shard) { return read_frames_buf(slurp(shard)); }
// the records of a shard as a sorted multiset; contribution records (types 5 and 7) depend on the reference set 𝔅 of the process and are left out
static std::vector<std::string> frame_set(const std::string& shard, bool with_contrib) {  // stage 5: with_contrib = false leaves out types 5 and 7; type 6 (R_WM) is always left out
  std::vector<std::string> v;
  for (auto& f : read_frames(shard)) if ((with_contrib || (f.type != 5 && f.type != 7)) && f.type != 6) v.push_back(std::string(1, (char)f.type) + f.pl);
  std::sort(v.begin(), v.end());
  return v;
}

static std::vector<std::string> frame_set_buf(const std::string& content, bool with_contrib) {
  std::vector<std::string> v;
  for (auto& f : read_frames_buf(content)) if ((with_contrib || (f.type != 5 && f.type != 7)) && f.type != 6) v.push_back(std::string(1, (char)f.type) + f.pl);
  std::sort(v.begin(), v.end());
  return v;
}

struct Lib {
  vrs_h* h = nullptr;
  bool open(const std::string& sdir, const std::string& idir) {
    vrs_opts o{}; o.sessions_dir = sdir.c_str(); o.index_dir = idir.c_str(); o.segwin = 1; o.budget = 258400;
    if (vrs_open(&o, &h) != 0) return false;
    while (vrs_ready(h) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return vrs_ready(h) == 1;
  }
  vrs_dbg stats() { vrs_dbg d; vrs_dbg_stats(h, &d); return d; }
  uint64_t hash(const std::string& p) { uint64_t x = 0; vrs_dbg_session_hash(h, p.c_str(), &x); return x; }
  void wait_lines(uint64_t n) {  // lines applied (the owner may be another thread)
    for (int i = 0; i < 20000 && (stats().lines_applied < n || stats().pend != 0); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::string asmb(const std::string& p, int* rc_out = nullptr) {
    char* j = nullptr; size_t n = 0; const int rc = vrs_assemble(h, p.c_str(), &j, &n);
    std::string o = j ? std::string(j, n) : std::string(); vrs_free(j);
    // the "us" field is the time of the call
    const size_t u = o.rfind(",\"us\":"); if (u != std::string::npos) o.resize(u);
    if (rc_out) *rc_out = rc; return o;
  }
  void close() { if (h) vrs_close(h); h = nullptr; }
};

static std::string rel_shard(const std::string& idir, const std::string& sdir, const std::string& path) { return idir + "/" + path.substr(sdir.size() + 1) + ".vx"; }

// feed the file line by line the way the rollout writer does: write the line, then hand it over
static void stream(Lib& lib, const std::string& path, const std::vector<Line>& L, size_t from, size_t to, int times) {
  for (size_t i = from; i < to; i++) {
    append_file(path, L[i].s + "\n");
    for (int t = 0; t < times; t++) vrs_insert(lib.h, path.c_str(), L[i].off, L[i].s.data(), L[i].s.size());
    vrs_written(lib.h, path.c_str(), L[i].off);  // the line is in the file (P-3')
  }
}

static int mode_idem(const std::string& src, const std::string& work) {
  const auto L = read_lines(src);
  fs::remove_all(work);
  const std::string name = "rollout-idem.jsonl";
  struct Res { vrs_dbg d; uint64_t hash; std::string shard; std::string ans; };
  auto run = [&](const std::string& tag, int times, bool second_pass) {
    const std::string sdir = work + "/s", idir = work + "/" + tag + "/i", path = sdir + "/2026/01/01/" + name;  // same path in every run: the turn seed hashes the path
    fs::remove_all(sdir); fs::create_directories(sdir + "/2026/01/01"); fs::create_directories(idir);
    Lib lib; check(lib.open(sdir, idir), tag + ": open (no files yet, ready at once)");
    write_file(path, "");
    stream(lib, path, L, 0, L.size(), times);
    lib.wait_lines(L.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Res r{lib.stats(), lib.hash(path), slurp(rel_shard(idir, sdir, path)), lib.asmb(path)};
    if (second_pass) {
      for (auto& l : L) vrs_insert(lib.h, path.c_str(), l.off, l.s.data(), l.s.size());  // every key again
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      Res r2{lib.stats(), lib.hash(path), slurp(rel_shard(idir, sdir, path)), lib.asmb(path)};
      check(r2.ans == r.ans && !r.ans.empty(), tag + ": second pass: assembled answer unchanged (" + std::to_string(r.ans.size()) + " bytes)");
      check(r2.d.items == r.d.items && r2.d.turns == r.d.turns && r2.d.lines_applied == r.d.lines_applied && r2.d.cells_hash == r.d.cells_hash,
            tag + ": second pass of the same keys: items " + std::to_string(r.d.items) + " turns " + std::to_string(r.d.turns) + " lines " + std::to_string(r.d.lines_applied) + " unchanged");
      check(r2.hash == r.hash, tag + ": second pass: session hash unchanged");
      check(r2.shard == r.shard && !r.shard.empty(), tag + ": second pass: shard byte-identical (" + std::to_string(r.shard.size()) + " bytes)");
    }
    lib.close();
    return r;
  };
  Res a = run("a_single", 1, true);
  Res b = run("b_double", 2, false);
  Res b2 = run("b2_single", 1, false);
  check(b2.hash == a.hash, "control: a second single-insert run in another directory: session hash == first run");
  check(a.d.items == b.d.items && a.d.turns == b.d.turns && a.d.lines_applied == b.d.lines_applied, "each line inserted twice in a row: same items / turns / lines as once");
  check(a.hash == b.hash, "each line inserted twice in a row: session hash == single insert");
  // The marks (type 6) are written once per batch and vrs_insert no longer waits for the batch (round 4, P-7), so where the batches fall is not fixed any more: the
  // records themselves (every type but the marks, the contributions of the other process state left out as before) must be the same multiset.
  check(frame_set_buf(a.shard, true) == frame_set_buf(b.shard, true) && !frame_set_buf(a.shard, true).empty(), "T5-3: each line inserted twice in a row: the shard holds the same records (all types, R_WM left out) as with a single insert (" + std::to_string(frame_set_buf(b.shard, true).size()) + " records)");
  check(a.d.cells_hash == b.d.cells_hash && a.d.cells_n == b.d.cells_n, "T5-3: each line inserted twice in a row: the cells (hash, count) equal the single insert's");
  check(a.ans == b.ans && !a.ans.empty(), "T5-3: each line inserted twice in a row: the assembled answer equals the single insert's byte for byte (" + std::to_string(a.ans.size()) + " bytes)");
  // restore: the file as the insert run left it, parsed from scratch, and again from the insert-built shard
  const std::string sdir = work + "/s", path = sdir + "/2026/01/01/" + name;
  const std::string i3 = work + "/c_restore_i";
  fs::create_directories(i3);
  Lib lib3; check(lib3.open(sdir, i3), "c: open on the finished file, no shard (full parse)");
  const uint64_t h3 = lib3.hash(path); const vrs_dbg d3 = lib3.stats();
  lib3.close();
  check(h3 == a.hash, "c: parse from scratch == insert-built tables (session hash)");
  check(d3.items == a.d.items && d3.turns == a.d.turns, "c: items / turns equal");
  check(frame_set(rel_shard(i3, sdir, path), false) == frame_set(rel_shard(work + "/a_single/i", sdir, path), false), "c: item / meta / task_started / turn records: same multiset as the insert-built shard");
  check(d3.cells_hash == a.d.cells_hash, "c: cells from a parse from scratch == insert-built cells");
  const std::string i1 = work + "/a_single/i";
  const auto sz1 = fs::file_size(rel_shard(i1, sdir, path));
  Lib lib4; check(lib4.open(sdir, i1), "d: open on the insert-built shard");
  const uint64_t h4 = lib4.hash(path); const vrs_dbg d4 = lib4.stats();
  lib4.close();
  check(d4.shards_loaded == 1 && d4.shards_rebuilt == 0, "d: shard accepted (loaded " + std::to_string(d4.shards_loaded) + ", rebuilt " + std::to_string(d4.shards_rebuilt) + ")");
  check(h4 == a.hash, "d: restored tables == insert-built tables (session hash)");
  check(fs::file_size(rel_shard(i1, sdir, path)) == sz1, "d: restart appended nothing to the shard");
  return g_fail;
}

static int mode_replay(const std::string& src, const std::string& work) {
  const auto L = read_lines(src);
  fs::remove_all(work);
  const std::string name = "rollout-replay.jsonl";
  // reference: one full parse
  uint64_t href = 0; std::vector<std::string> sref; vrs_dbg dref{};
  {
    const std::string sdir = work + "/s", idir = work + "/full/i", path = sdir + "/2026/01/01/" + name;
    fs::remove_all(sdir);
    write_file(path, prefix_text(L, L.size()));
    fs::create_directories(idir);
    Lib lib; check(lib.open(sdir, idir), "reference: full parse");
    href = lib.hash(path); dref = lib.stats(); sref = frame_set(rel_shard(idir, sdir, path), false);
    lib.close();
  }
  // cut points: where the parse state is least trivial
  std::vector<size_t> cuts = {0, 1, 2};
  size_t nts = 0;
  for (size_t i = 0; i < L.size(); i++) {
    const bool ts = L[i].s.find("\"payload\":{\"type\":\"task_started\"") != std::string::npos;
    const bool tc = L[i].s.find("\"payload\":{\"type\":\"task_complete\"") != std::string::npos;
    const bool ws = L[i].s.find("\"type\":\"world_state\"") != std::string::npos;
    if ((ts || tc || ws) && nts < 24) { cuts.push_back(i); cuts.push_back(i + 1); if (ts) nts++; }
  }
  for (size_t k = 1; k < 12; k++) cuts.push_back(L.size() * k / 12);
  cuts.push_back(L.size() - 1); cuts.push_back(L.size());
  std::sort(cuts.begin(), cuts.end()); cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  int ok_all = 0;
  for (size_t cut : cuts) {
    if (cut > L.size()) continue;
    const std::string tag = "cut" + std::to_string(cut);
    const std::string sdir = work + "/s", idir = work + "/" + tag + "/i", path = sdir + "/2026/01/01/" + name;
    fs::remove_all(sdir);
    write_file(path, prefix_text(L, cut));
    fs::create_directories(idir);
    { Lib lib; if (!lib.open(sdir, idir)) { check(false, tag + ": open 1"); continue; } lib.close(); }  // shard of the prefix
    std::string rest; for (size_t i = cut; i < L.size(); i++) rest += L[i].s + "\n";
    append_file(path, rest);                                                                               // the file grows while the library is down
    Lib lib; if (!lib.open(sdir, idir)) { check(false, tag + ": open 2"); continue; }
    const uint64_t h = lib.hash(path); const vrs_dbg d = lib.stats();
    const auto s = frame_set(rel_shard(idir, sdir, path), false);
    lib.close();
    const bool good = h == href && d.items == dref.items && d.turns == dref.turns && s == sref && (cut == 0 || d.shards_loaded == 1 || cut < 1) && d.shards_rebuilt == 0;
    if (!good) std::printf("   cut %zu: hash %s items %llu/%llu turns %llu/%llu shard-set %s loaded %llu rebuilt %llu\n", cut, h == href ? "same" : "DIFF", (unsigned long long)d.items,
                           (unsigned long long)dref.items, (unsigned long long)d.turns, (unsigned long long)dref.turns, s == sref ? "same" : "DIFF",
                           (unsigned long long)d.shards_loaded, (unsigned long long)d.shards_rebuilt);
    ok_all += good;
    check(good, tag + " (prefix of " + std::to_string(cut) + " of " + std::to_string(L.size()) + " lines, rest appended, restart): tables and shard == full parse");
  }
  std::printf("replay: %d of %zu cut points equal to the full parse\n", ok_all, cuts.size());
  // a damaged shard must fall back to a full parse: flip bytes in the middle, truncate the tail
  {
    const std::string sdir = work + "/s", idir = work + "/full/i", path = sdir + "/2026/01/01/" + name;
    const std::string sh = rel_shard(idir, sdir, path);
    fs::remove_all(sdir); write_file(path, prefix_text(L, L.size()));
    std::string b = slurp(sh);
    std::string torn = b.substr(0, b.size() - 7);  // a torn last record
    write_file(sh, torn);
    Lib lib; check(lib.open(sdir, idir), "torn shard: open");
    const uint64_t h = lib.hash(path); const vrs_dbg d = lib.stats(); lib.close();
    check(h == href && d.items == dref.items, "torn shard (last record cut): tables == full parse (loaded " + std::to_string(d.shards_loaded) + ", rebuilt " + std::to_string(d.shards_rebuilt) + ", damaged records " + std::to_string(d.shard_corrupt) + ")");
    std::string b2 = b;
    for (size_t k = b2.size() / 3; k < b2.size() / 3 + 40 && k < b2.size(); k++) b2[k] ^= 0x5a;
    write_file(sh, b2);
    Lib lib2; check(lib2.open(sdir, idir), "damaged shard: open");
    const uint64_t h2 = lib2.hash(path); const vrs_dbg d2 = lib2.stats(); lib2.close();
    check(h2 == href && d2.items == dref.items, "damaged shard (40 bytes flipped): tables == full parse (loaded " + std::to_string(d2.shards_loaded) + ", rebuilt " + std::to_string(d2.shards_rebuilt) + ", damaged records " + std::to_string(d2.shard_corrupt) + ")");
  }
  return g_fail;
}

static int mode_ooo(const std::string& src, const std::string& work) {
  const auto L = read_lines(src);
  fs::remove_all(work);
  const std::string name = "rollout-ooo.jsonl";
  uint64_t href = 0;
  {
    const std::string sdir = work + "/s", idir = work + "/full/i", path = sdir + "/2026/01/01/" + name;
    fs::remove_all(sdir); write_file(path, prefix_text(L, L.size())); fs::create_directories(idir);
    Lib lib; lib.open(sdir, idir); href = lib.hash(path); lib.close();
  }
  const std::string sdir = work + "/s", idir = work + "/o/i", path = sdir + "/2026/01/01/" + name;
  fs::remove_all(sdir); fs::create_directories(sdir + "/2026/01/01"); fs::create_directories(idir);
  Lib lib; check(lib.open(sdir, idir), "open");
  write_file(path, prefix_text(L, L.size()));  // the file is whole; the lines arrive backwards (every one beyond a gap until the file read fills it)
  for (size_t i = std::min<size_t>(L.size(), 60); i-- > 0;) { vrs_insert(lib.h, path.c_str(), L[i].off, L[i].s.data(), L[i].s.size()); vrs_written(lib.h, path.c_str(), L[i].off); }
  lib.wait_lines(L.size());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  check(lib.hash(path) == href, "reverse order (first 60 lines backwards, file present): tables == in-order parse");
  check(lib.stats().lines_applied == L.size(), "every line applied exactly once (" + std::to_string(lib.stats().lines_applied) + " of " + std::to_string(L.size()) + ")");
  lib.close();
  return g_fail;
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: vrs_t_insert idem|replay|ooo <source rollout> <work dir>\n"); return 2; }
  const std::string m = argv[1];
  int rc = 2;
  if (m == "idem") rc = mode_idem(argv[2], argv[3]);
  else if (m == "replay") rc = mode_replay(argv[2], argv[3]);
  else if (m == "ooo") rc = mode_ooo(argv[2], argv[3]);
  std::printf("%s: %d failure(s)\n", m.c_str(), g_fail);
  return rc;
}
