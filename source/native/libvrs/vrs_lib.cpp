// libvrs - the VRS synapse assembler (vrs9syn42f = 42e + VRS_클로드코드_v1 4판 window) as an in-process library.
// L3 of VRS_코덱스일체화_v1.md (§4 P-1..P-5, §5 E3-1). Derived from vrs9syn42f.cpp: code that is unchanged is copied verbatim,
// every changed place carries a "libvrs:" remark. Concurrency is atomics and CAS only: the library has no waiting primitive of
// the operating-system kind, no process exit and no environment reads; every option arrives in vrs_opts.
#include "vrs.h"
#include "vrs_workset.h"
#include "vrs_witness.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <climits>
#include <charconv>
#include "vrs_plat.h"  // platform layer (VRS_MCP_libvrs_v1.md 13.2): the POSIX headers and calls of the Linux build, Win32 on Windows
#include "vrs_tail_buffer.h"  // N-D: thread-owned allocation storage, unchanged input bytes
#include "vrs_handle_cache.h"
#include "swegca_architecture/evidence_rules.hpp"
#include "swegca_architecture/sha256.hpp"

namespace fs = std::filesystem;
namespace sa = swegca::architecture;
namespace kernel = sa::kernel;
using sa::EvidencePolicy;
using sa::make_evidence_rules;
using sa::standard_normal_quantile;

namespace {

constexpr uint64_t TH_SEC = 1ull << 20;  // theta unit = 2^-20 s (vrs9_types.hpp)
constexpr size_t SEG_W = 8;
constexpr uint32_t NONE32 = UINT32_MAX;
enum Kind : uint8_t { K_USER = 0, K_ASSIST = 1, K_DEV = 2, K_REASON = 3, K_FC = 4, K_CTC = 6, K_FCO = 5, K_CTCO = 7, K_OTHER = 8 };

// ---------------- containers: chunks never move, nothing of the full size is allocated up front (P-3) ----------------
// A list with one appender and many readers. Chunk k holds 32 << k elements; the chunk table is part of the object.
template <class T> struct SVec {
  static constexpr int NCH = 28;
  std::atomic<T*> ch[NCH];
  std::atomic<uint32_t> n{0};
  SVec() { for (auto& c : ch) c.store(nullptr, std::memory_order_relaxed); }
  SVec(const SVec&) = delete;
  SVec& operator=(const SVec&) = delete;
  ~SVec() { for (auto& c : ch) delete[] c.load(std::memory_order_relaxed); }
  static void loc(size_t i, int& k, size_t& o) { const size_t q = i / 32 + 1; k = 63 - __builtin_clzll((unsigned long long)q); o = i - 32 * ((size_t(1) << k) - 1); }
  const T& operator[](size_t i) const { int k; size_t o; loc(i, k, o); return ch[k].load(std::memory_order_acquire)[o]; }
  size_t size() const { return n.load(std::memory_order_acquire); }
  void push_back(const T& v) {  // the owner of the list only
    const size_t i = n.load(std::memory_order_relaxed);
    int k; size_t o; loc(i, k, o);
    T* c = ch[k].load(std::memory_order_relaxed);
    if (!c) { c = new T[size_t(32) << k](); ch[k].store(c, std::memory_order_release); }
    c[o] = v;
    n.store(uint32_t(i + 1), std::memory_order_release);
  }
};

// A table addressed by a claimed index. The chunk table is a zeroed array of 2^(CAPB-CB) pointers; a chunk is made by CAS the first time
// an index in it is touched and never moves again.
template <class T, int CB, int CAPB> struct GVec {
  static constexpr size_t CH = size_t(1) << CB;
  static constexpr size_t NDIR = size_t(1) << (CAPB - CB);
  std::unique_ptr<std::atomic<T*>[]> dir;
  GVec() : dir(new std::atomic<T*>[NDIR]()) {}
  GVec(const GVec&) = delete;
  GVec& operator=(const GVec&) = delete;
  ~GVec() { for (size_t d = 0; d < NDIR; d++) delete[] dir[d].load(std::memory_order_relaxed); }
  T& operator[](size_t i) const { return dir[i >> CB].load(std::memory_order_acquire)[i & (CH - 1)]; }
  T& ensure(size_t i) {
    auto& d = dir[i >> CB];
    T* c = d.load(std::memory_order_acquire);
    if (!c) {
      T* nc = new T[CH]();
      T* e = nullptr;
      if (d.compare_exchange_strong(e, nc, std::memory_order_acq_rel)) c = nc; else { delete[] nc; c = e; }
    }
    return c[i & (CH - 1)];
  }
};

// Insert-only hash table of bucket chains. Nodes never move and are never removed; a bucket is extended by CAS at its head.
template <class V> struct LFMap {
  struct Node { uint64_t key = 0; std::atomic<Node*> next{nullptr}; V val{}; };
  static constexpr int DB = 22, CB = 12;  // 4M buckets in chunks of 4096
  static constexpr size_t ND = size_t(1) << (DB - CB), CHN = size_t(1) << CB;
  std::unique_ptr<std::atomic<std::atomic<Node*>*>[]> dir;
  LFMap() : dir(new std::atomic<std::atomic<Node*>*>[ND]()) {}
  LFMap(const LFMap&) = delete;
  LFMap& operator=(const LFMap&) = delete;
  ~LFMap() {
    for (size_t d = 0; d < ND; d++) {
      std::atomic<Node*>* c = dir[d].load(std::memory_order_relaxed);
      if (!c) continue;
      for (size_t i = 0; i < CHN; i++) for (Node* n = c[i].load(std::memory_order_relaxed); n;) { Node* x = n->next.load(std::memory_order_relaxed); delete n; n = x; }
      delete[] c;
    }
  }
  static size_t slot(uint64_t key) { return size_t((key * 0x9E3779B97F4A7C15ull) >> (64 - DB)); }
  Node* find(uint64_t key) const {
    const size_t b = slot(key);
    std::atomic<Node*>* c = dir[b >> CB].load(std::memory_order_acquire);
    if (!c) return nullptr;
    for (Node* n = c[b & (CHN - 1)].load(std::memory_order_acquire); n; n = n->next.load(std::memory_order_acquire)) if (n->key == key) return n;
    return nullptr;
  }
  Node* get(uint64_t key) {
    const size_t b = slot(key);
    auto& d = dir[b >> CB];
    std::atomic<Node*>* c = d.load(std::memory_order_acquire);
    if (!c) {
      auto* nc = new std::atomic<Node*>[CHN]();
      std::atomic<Node*>* e = nullptr;
      if (d.compare_exchange_strong(e, nc, std::memory_order_acq_rel)) c = nc; else { delete[] nc; c = e; }
    }
    std::atomic<Node*>& bk = c[b & (CHN - 1)];
    Node* head = bk.load(std::memory_order_acquire);
    Node* nn = nullptr;
    for (;;) {
      for (Node* n = head; n; n = n->next.load(std::memory_order_acquire)) if (n->key == key) { delete nn; return n; }
      if (!nn) { nn = new Node; nn->key = key; }
      nn->next.store(head, std::memory_order_relaxed);
      if (bk.compare_exchange_weak(head, nn, std::memory_order_release, std::memory_order_acquire)) return nn;
    }
  }
};

// ---------------- libvrs 5 (VRS_코덱스일체화_v1.md §7): sparse tables over the ordinal space (P-11) ----------------
// An index space of 2^24 (the ordinal of a line, the position of an item in its file) in three levels of 256. Nothing is made for a range nobody
// touched; a page is made by CAS the first time an index in it is written and never moves again, so readers need no protection.
template <class T> struct Sparse {
  static constexpr int PB = 8, MB = 8, TB = 8;
  static constexpr size_t PG = size_t(1) << PB, MD = size_t(1) << MB, TP = size_t(1) << TB, LIM = size_t(1) << (PB + MB + TB);
  std::atomic<std::atomic<std::atomic<T*>*>*> root{nullptr};  // TP pointers to mid blocks of MD pointers to pages of PG entries
  Sparse() = default;
  Sparse(const Sparse&) = delete;
  Sparse& operator=(const Sparse&) = delete;
  ~Sparse() {
    auto* r = root.load(std::memory_order_relaxed);
    if (!r) return;
    for (size_t a = 0; a < TP; a++) {
      auto* m = r[a].load(std::memory_order_relaxed);
      if (!m) continue;
      for (size_t b = 0; b < MD; b++) delete[] m[b].load(std::memory_order_relaxed);
      delete[] m;
    }
    delete[] r;
  }
  T* find(size_t i) const {
    if (i >= LIM) return nullptr;
    auto* r = root.load(std::memory_order_acquire);
    if (!r) return nullptr;
    auto* m = r[i >> (PB + MB)].load(std::memory_order_acquire);
    if (!m) return nullptr;
    T* p = m[(i >> PB) & (MD - 1)].load(std::memory_order_acquire);
    return p ? p + (i & (PG - 1)) : nullptr;
  }
  T& at(size_t i) {  // i < LIM
    auto* r = root.load(std::memory_order_acquire);
    if (!r) {
      auto* nr = new std::atomic<std::atomic<T*>*>[TP]();
      std::atomic<std::atomic<T*>*>* e = nullptr;
      if (root.compare_exchange_strong(e, nr, std::memory_order_acq_rel)) r = nr; else { delete[] nr; r = e; }
    }
    auto& ma = r[i >> (PB + MB)];
    auto* m = ma.load(std::memory_order_acquire);
    if (!m) {
      auto* nm = new std::atomic<T*>[MD]();
      std::atomic<T*>* e = nullptr;
      if (ma.compare_exchange_strong(e, nm, std::memory_order_acq_rel)) m = nm; else { delete[] nm; m = e; }
    }
    auto& pa = m[(i >> PB) & (MD - 1)];
    T* p = pa.load(std::memory_order_acquire);
    if (!p) {
      T* np = new T[PG]();
      T* e = nullptr;
      if (pa.compare_exchange_strong(e, np, std::memory_order_acq_rel)) p = np; else { delete[] np; p = e; }
    }
    return p[i & (PG - 1)];
  }
};

// A list of ids written into cells that the writers choose themselves (P-11: the cell of an item is its position in the file, found by a count, so the cells
// are written in any order and never collide). 0 = not written. A reader reads up to the first cell that is not written.
struct Cells {
  Sparse<std::atomic<uint32_t>> c;
  mutable std::atomic<uint32_t> okn{0};
  void set(size_t i, uint32_t v) { c.at(i).store(v, std::memory_order_release); }
  uint32_t get(size_t i) const { const auto* p = c.find(i); return p ? p->load(std::memory_order_acquire) : 0; }
  uint32_t operator[](size_t i) const { return get(i); }
  size_t size() const {  // the written prefix
    size_t n = okn.load(std::memory_order_relaxed);
    while (get(n)) n++;
    uint32_t o = okn.load(std::memory_order_relaxed);
    while (n > o && !okn.compare_exchange_weak(o, uint32_t(n), std::memory_order_relaxed)) {}
    return n;
  }
};

// Fenwick tree over the ordinals of one file (P-11): counts are added when a line is put in and read when it becomes visible. Two 64-bit lanes per node:
// a = items (low 32) | user items (high 32); b = task_started lines (low 32). Inner trees of 256 ordinals and one tree over the pages; both are sparse.
struct OrdNode { std::atomic<uint64_t> a{0}, b{0}; };
struct OrdTree {
  static constexpr uint32_t IN = 256, TOPN = 65536;
  Sparse<OrdNode> inner;  // page p, inner Fenwick node k (1..IN) at p * IN + k - 1
  Sparse<OrdNode> top;    // page Fenwick node k (1..TOPN) at k - 1
  void add(uint64_t ord, uint64_t da, uint64_t db) {
    const uint64_t p = ord >> 8;
    for (uint32_t k = uint32_t(ord & (IN - 1)) + 1; k <= IN; k += k & (0u - k)) {
      OrdNode& n = inner.at(p * IN + k - 1);
      if (da) n.a.fetch_add(da, std::memory_order_seq_cst);
      if (db) n.b.fetch_add(db, std::memory_order_seq_cst);
    }
    for (uint32_t k = uint32_t(p) + 1; k <= TOPN; k += k & (0u - k)) {
      OrdNode& n = top.at(k - 1);
      if (da) n.a.fetch_add(da, std::memory_order_seq_cst);
      if (db) n.b.fetch_add(db, std::memory_order_seq_cst);
    }
  }
  void prefix(uint64_t ord, uint64_t& a, uint64_t& b) const {  // sums over the ordinals < ord
    a = b = 0;
    const uint64_t p = ord >> 8;
    for (uint32_t k = uint32_t(ord & (IN - 1)); k > 0; k -= k & (0u - k)) {
      if (const OrdNode* n = inner.find(p * IN + k - 1)) { a += n->a.load(std::memory_order_seq_cst); b += n->b.load(std::memory_order_seq_cst); }
    }
    for (uint32_t k = uint32_t(p); k > 0; k -= k & (0u - k)) {
      if (const OrdNode* n = top.find(k - 1)) { a += n->a.load(std::memory_order_seq_cst); b += n->b.load(std::memory_order_seq_cst); }
    }
  }
  // the ordinal of the m-th task_started line (m >= 1), UINT64_MAX when there are fewer
  uint64_t select_start(uint32_t m) const {
    uint32_t pos = 0, rem = m;
    for (uint32_t step = TOPN; step; step >>= 1) {
      const uint32_t nx = pos + step;
      if (nx <= TOPN) {
        const OrdNode* n = top.find(nx - 1);
        const uint32_t c = n ? uint32_t(n->b.load(std::memory_order_seq_cst) & 0xffffffffu) : 0;
        if (c < rem) { pos = nx; rem -= c; }
      }
    }
    if (pos >= TOPN) return UINT64_MAX;
    const uint64_t p = pos;
    uint32_t ip = 0;
    for (uint32_t step = IN; step; step >>= 1) {
      const uint32_t nx = ip + step;
      if (nx <= IN) {
        const OrdNode* n = inner.find(p * IN + nx - 1);
        const uint32_t c = n ? uint32_t(n->b.load(std::memory_order_seq_cst) & 0xffffffffu) : 0;
        if (c < rem) { ip = nx; rem -= c; }
      }
    }
    return p * IN + ip;
  }
};

// ---------------- text helpers (verbatim from 42f) ----------------
uint64_t fnv(const char* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 1099511628211ull; }
  h ^= h >> 31; h *= 0x9E3779B97F4A7C15ull; h ^= h >> 29;
  return h;
}

// value of the first "key":"value" string after position from (raw, escapes kept)
std::string sval(const std::string& s, const char* key, size_t from = 0) {
  std::string pat = std::string("\"") + key + "\":\"";
  size_t p = s.find(pat, from);
  if (p == std::string::npos) return "";
  p += pat.size();
  size_t q = p;
  while (q < s.size() && !(s[q] == '"' && s[q - 1] != '\\')) q++;
  return s.substr(p, q - p);
}

// numeric value after "key": (UINT64_MAX when absent)
uint64_t numval(const std::string& s, const char* key) {
  std::string pat = std::string("\"") + key + "\":";
  size_t p = s.find(pat);
  if (p == std::string::npos) return UINT64_MAX;
  return strtoull(s.c_str() + p + pat.size(), nullptr, 10);
}

// days from civil (Howard Hinnant)
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
uint64_t parse_ts(const std::string& t) {  // 2026-10-03T02:47:14.483Z -> theta (2^-20 s)
  if (t.size() < 19) return 0;
  int64_t Y = atoi(t.substr(0, 4).c_str()), M = atoi(t.substr(5, 2).c_str()), D = atoi(t.substr(8, 2).c_str());
  int64_t h = atoi(t.substr(11, 2).c_str()), mi = atoi(t.substr(14, 2).c_str()), se = atoi(t.substr(17, 2).c_str());
  int64_t ms = 0;
  if (t.size() > 20 && t[19] == '.') { std::string f = t.substr(20, 3); while (f.size() < 3) f += '0'; ms = atoi(f.c_str()); }
  int64_t sec = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + se;
  return (uint64_t)sec * TH_SEC + (uint64_t)ms * TH_SEC / 1000;
}

// ================= synapse assembly v1 (VRS_시냅스조립_v1.md) =================
// Cue tokens: the same function as cueindex.cpp tokenize (turnvrsd cue_set), copied verbatim.
uint64_t ci_mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return x; }
uint64_t tok_hash(char tag, const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull ^ uint64_t(uint8_t(tag));
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
  return ci_mix(h ^ n);
}
void put_utf8(uint32_t cp, std::string& o) {
  if (cp < 0x80) o += char(cp);
  else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
  else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
}
bool cjk(uint32_t c) { return (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF); }
bool b64c(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='; }
int hexv(uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
template <class F> void tokenize(const uint8_t* p, size_t n, F&& emit) {
  std::vector<uint32_t> run;
  std::string w, s;
  auto flush_run = [&] {
    if (run.size() == 1) { s.clear(); put_utf8(run[0], s); emit(tok_hash('K', (const uint8_t*)s.data(), s.size())); }
    for (size_t i = 0; i + 1 < run.size(); ++i) {
      s.clear(); put_utf8(run[i], s); put_utf8(run[i + 1], s);
      emit(tok_hash('K', (const uint8_t*)s.data(), s.size()));
    }
    run.clear();
  };
  size_t b64_end = 0;
  for (size_t i = 0; i < n;) {
    const uint8_t c = p[i];
    if (c == '\\' && i + 1 < n) {
      if (p[i + 1] == 'u' && i + 5 < n) {
        int v = 0; bool ok = true;
        for (int k = 2; k < 6; ++k) { const int h = hexv(p[i + k]); if (h < 0) { ok = false; break; } v = v * 16 + h; }
        if (ok) { i += 6; if (cjk(uint32_t(v))) run.push_back(uint32_t(v)); else flush_run(); continue; }
      }
      flush_run(); i += 2; continue;
    }
    if (c < 0x80) {
      flush_run();
      const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      if (!word) { ++i; continue; }
      if (i >= b64_end) {
        size_t j = i; while (j < n && b64c(p[j])) ++j;
        if (j - i >= 256) { i = j; continue; }
        b64_end = j;
      }
      size_t j = i; w.clear();
      while (j < n && ((p[j] >= 'a' && p[j] <= 'z') || (p[j] >= 'A' && p[j] <= 'Z') || (p[j] >= '0' && p[j] <= '9') || p[j] == '_')) {
        w += char(p[j] >= 'A' && p[j] <= 'Z' ? p[j] + 32 : p[j]); ++j;
      }
      if (w.size() >= 2 && w.size() <= 40) emit(tok_hash('W', (const uint8_t*)w.data(), w.size()));
      i = j; continue;
    }
    uint32_t cp = 0; size_t len = 1;
    if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 31u) << 6) | (p[i + 1] & 63u); len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 15u) << 12) | ((p[i + 1] & 63u) << 6) | (p[i + 2] & 63u); len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 7u) << 18) | ((p[i + 1] & 63u) << 12) | ((p[i + 2] & 63u) << 6) | (p[i + 3] & 63u); len = 4; }
    if (cjk(cp)) run.push_back(cp); else flush_run();
    i += len;
  }
  flush_run();
}
std::vector<uint64_t> cue_set(const std::string& s) {
  std::vector<uint64_t> h;
  tokenize(reinterpret_cast<const uint8_t*>(s.data()), s.size(), [&](uint64_t v) { h.push_back(v); });
  std::sort(h.begin(), h.end()); h.erase(std::unique(h.begin(), h.end()), h.end());
  return h;
}
std::string clip_chars(const std::string& s, size_t n) {  // first n code points (turnvrsd clip_chars)
  size_t cps = 0, i = 0;
  for (; i < s.size(); ++i) if ((uint8_t(s[i]) & 0xC0) != 0x80 && cps++ == n) break;
  return s.substr(0, i);
}
std::string json_string(std::string_view s, size_t at) {  // turnvrsd json_string; at = just past the opening quote
  std::string o;
  for (size_t i = at; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') break;
    if (c != '\\' || i + 1 >= s.size()) { o += c; continue; }
    const char e = s[++i];
    if (e == 'n') o += '\n'; else if (e == 't') o += '\t'; else if (e == 'r') {} else if (e == 'u' && i + 4 < s.size()) {
      uint32_t cp = uint32_t(std::strtoul(std::string(s.substr(i + 1, 4)).c_str(), nullptr, 16)); i += 4;
      if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
        const uint32_t lo = uint32_t(std::strtoul(std::string(s.substr(i + 3, 4)).c_str(), nullptr, 16));
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6;
      }
      put_utf8(cp, o);
    } else o += e;
  }
  return o;
}
std::string_view find_field(const std::string& line, const char* key, size_t limit) {  // turnvrsd find_field
  const std::string_view v(line.data(), std::min(line.size(), limit));
  const auto at = v.find(key);
  if (at == std::string_view::npos) return {};
  const auto b = at + std::strlen(key), e = v.find('"', b);
  return e == std::string_view::npos ? std::string_view{} : v.substr(b, e - b);
}

// observer v11 constants and the shared plain data (verbatim; libvrs: ObsY gets the flag `bad`, lemma E no longer ends the process)
struct A0Val; struct Contrib; struct DepChunk;
struct K5U; struct K5C; struct K5UCand; struct K5CCand;  // K5 3판 (vrs_k5.cpp): values of their own, lib7 values are only read
// libvrs 5 (VRS_코덱스일체화_v1.md §7, D9 / P-9): everything that depends on 𝔅 carries the fingerprint of the window it was made from, phi(y) = (wn, sum of the
// identity hashes of the turns at the 𝔅 positions [k - wn, k)); the prefix fingerprint F(c) of the 4th stage is gone.
// observer v11 constants (obs1/proof_v11.md; turnvrsd)
constexpr uint32_t OBS_M = 99;
constexpr uint32_t OBS_W = 1024;  // D9 (VRS_코덱스일체화_v1.md §7.1): observer window W
constexpr double OBS_CONF = 0.9;
constexpr uint64_t OBS_KEY = 0x0B5E7A11E9A5D11Eull;
constexpr int KD = 64, KO = 64, KC = 16, KV = 8, KF = 6, NCELL = KD + KO + KC + KV + KF;  // L1 v7 D4⁗ far distance 64 < d < 4096, floor(log2 d) 6..11
constexpr size_t FAR_MAX = 4096;  // 7판 LWALK
constexpr int NP = 3, NC2 = NP * NCELL, NS = 8, NC3 = NC2 * NS, NCA = NC2 + NC3;  // L1 v14 D5⁶: parts u, a, n; coarse (key, part) at
// key + part * NCELL, fine (key, part, class) at NC2 + key + part * NCELL + class * NC2 (E2-27)
constexpr uint32_t RARE_F = 8;  // D4″ F
struct ObsY {
  // D9 / P-9: the window of y = the turns of 𝔅 at positions [k - wn, k), wn = min(k, W); `phi` = its fingerprint (wn, sum of the identity hashes of the window).
  // Floyd's draw is over the positions inside the window (0 .. wn-1), so a turn that comes in below the window moves k and the window but changes nothing here.
  uint32_t n = 0, src = 0, med = 0, max = 0, m0 = 0, k = 0, wn = 0; double tau = 0; bool bad = false; uint64_t phi = 0;
  std::optional<kernel::EvidenceRules> rules;  // empty = abstain before the core
  std::vector<uint32_t> R;  // R_y as positions inside the window, sorted (v2 D5′: the reference turn is drawn outside it)
};
constexpr uint64_t OBS_REF = 0x5EF7A11C0FFEE5EDull;  // v2 D5′ reference-turn stream
struct TurnRec {
  uint64_t o0 = 0, o1 = 0; uint32_t sg = 0, ord0 = 0, ord1 = 0, pos0 = 0, pos1 = 0, tpos = 0;
  uint64_t th0 = 0, the = 0, seed = 0;
  bool base = false;
  std::vector<uint32_t> vi;  // view items: request items, then the final item
  std::vector<uint64_t> C, D; std::vector<uint32_t> len;  // observer v11 turn data
  std::vector<uint64_t> Q;  // D4″ Q(x) = K(clip(request, 1500))
  std::vector<uint64_t> Q1; uint64_t tq1 = 0;  // L2 v9 (R04): Q as of the first utterance item, and that item's θ
  std::vector<uint64_t> Nc;  // L1 v14 D3‴: N(y) = K(clip(text of VI^n(y), 2500)), sorted
  std::atomic<A0Val*> a0{nullptr}; std::atomic<Contrib*> ctr{nullptr}; Contrib* cand = nullptr;  // D4‴ A₀(x); ctr = the contribution unit of x (D11); cand = restored units
  uint32_t n = 0, src = 0, nu = 0;  // nu: lines of the request side (they come first) (D2″)
  std::atomic<ObsY*> obs{nullptr};
  std::atomic<DepChunk*> deps{nullptr};   // reverse list: the turns whose unit holds this turn among its Y (P-10)
  std::atomic<uint8_t> regd{0};           // the turn has put itself in the lists of its Y
  // K5 3판 (VRS_K5_query_v1.md §14, vrs_k5.cpp): k5b = in 𝔅; k5in = 0 not a K5 memory yet, 1 in the base, 2 claimed by the overlay, 3 in the overlay
  std::vector<uint64_t> k5set;            // K(V(y)) sorted (written before the turn is claimed)
  std::atomic<uint8_t> k5b{0}, k5in{0};
  std::atomic<K5U*> k5u{nullptr}; std::atomic<K5C*> k5c{nullptr};
  std::atomic<K5UCand*> k5ucand{nullptr}; std::atomic<K5CCand*> k5ccand{nullptr};  // restored units
  std::atomic<DepChunk*> k5rev{nullptr};  // rev(y): the turns whose U held this turn
  std::atomic<uint64_t> k5wphi{0}, k5wchk{0};  // the window fingerprint while 𝔅 has not moved
};
struct A0Val { std::vector<uint32_t> v, Yb; uint64_t phi = 0; std::atomic<uint64_t> vstamp{0}; };  // A0(x), the turns of BC(x) it was made over, and the sum of their window fingerprints
struct CEnt { uint16_t cell; uint8_t fl; uint8_t pad; uint32_t cnt; };  // n identical cell adds: (cell, alpha | beta << 1)
struct Contrib {  // D11: the contribution unit of a counted turn: the cells it added (P-1) and the set it was counted over
  uint64_t phi = 0, ny = 0;  // sum over Y of the window fingerprints of the turns of Y; |Y| (a restored unit has no Y yet)
  uint32_t root = 0; std::vector<CEnt> e;
  std::vector<uint32_t> Y; std::vector<uint64_t> ph;  // Y: the turns whose observers the unit depends on, sorted; ph: their window fingerprints when it was made
  Contrib* next = nullptr;
};
struct DepChunk {  // insert-only list of turn ids (turn ids start at 1; 0 = slot not written yet)
  std::atomic<uint32_t> n{0}; std::atomic<uint32_t> id[14]; std::atomic<DepChunk*> next{nullptr};
  DepChunk() { for (auto& a : id) a.store(0, std::memory_order_relaxed); }
};

// the fields of a closed turn that turn_text derives from its lines (D8). libvrs: o0 = byte offset of the task_started line (the turn's key).
struct TurnParse {
  uint32_t ord0 = 0, ordl = 0, pos0 = 0, fin = UINT32_MAX;
  uint64_t th0 = 0, o0 = 0;
  std::string user, final_msg, last_assistant, first_assistant;
  std::vector<uint32_t> req;
  std::vector<uint64_t> q1; uint64_t tq1 = 0;
  std::vector<uint32_t> loose;  // E2-33: the event items of the turn (VI^n)
  std::string ntext;           // their texts (N(y), D3‴)
};
// D8 turn aggregate (VRS_코덱스일체화_v1.md §5.6): per turn start, the min / max / exists / o-sorted parts the turn fields are made of. libvrs 5: built from the
// lines of the turn when it closes (or, for the open turn, from the lines up to the end of the processed prefix), whatever the order they are taken in.
struct AgIn { uint32_t item = UINT32_MAX; uint8_t kind = 0; uint64_t th = 0; std::string text; bool agent = false; };  // a user message or agent_message (a candidate input line)
struct AgAs { std::string text; uint32_t item = UINT32_MAX; };                                                          // an assistant message
struct AgLo { uint32_t item = UINT32_MAX; std::string nt; };                                                            // an event item of the turn
struct TurnAgg {
  uint64_t o0 = 0, th0 = 0; uint32_t ord0 = 0, pos0 = 0, ordl = 0;
  uint64_t W = UINT64_MAX;                       // min o over the working lines (assistant messages, response items that are neither message nor agent_message)
  std::map<uint64_t, AgIn> in; std::map<uint64_t, AgAs> as; std::map<uint64_t, AgLo> lo;   // by o
  std::map<uint64_t, uint32_t> calls; std::set<uint64_t> outs;                            // calls by call hash; the set of calls that have an output
};
struct Cell { std::atomic<uint64_t> an{0}, ab{0}; std::atomic<uint32_t> lin1{0}, lin2{0}; };
// L2 v3 cue lists: (session, cue) -> the session's turns whose view has the cue, in tpos order. libvrs 5: the turns of a session close in any order, so a
// node is put in at its place by CAS (the list stays sorted for the readers, who never wait). The 2^26-slot table is a chunked hash.
struct PNode { uint32_t tpos = 0; std::atomic<PNode*> next{nullptr}; };
struct PSlot {
  std::atomic<PNode*> head{nullptr}, tail{nullptr};  // tail: a hint (the largest tpos so far)
  PSlot() = default;
  PSlot(const PSlot&) = delete;
  PSlot& operator=(const PSlot&) = delete;
  ~PSlot() { for (PNode* n = head.load(std::memory_order_relaxed); n;) { PNode* x = n->next.load(std::memory_order_relaxed); delete n; n = x; } }
};
uint64_t pt_key(uint64_t h, uint32_t sg) { const uint64_t k = ci_mix(h ^ (uint64_t(sg) * 0x9E3779B97F4A7C15ull)); return k ? k : 1; }
using Cuts = std::vector<std::pair<uint32_t, uint32_t>>;  // lineage pieces (session, turns cut)
struct CallVal { std::atomic<uint32_t> head{NONE32}; };  // libvrs 5: items of one (session, call_id), a stack chained by Item::nxt

// ---- L1 v13 D6⁶ current state instructions I(t, Λ) (VRS_시냅스조립_v1.md §48, §49) ----
// skip one JSON value starting at s[i] (whitespace first); returns the index after it
size_t json_skip(const std::string& s, size_t i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++;
  if (i >= s.size()) return i;
  if (s[i] == '"') { i++; while (i < s.size() && s[i] != '"') { if (s[i] == '\\') i++; i++; } return i + 1; }
  if (s[i] == '{' || s[i] == '[') {
    int dep = 0; bool str = false;
    for (; i < s.size(); i++) {
      const char c = s[i];
      if (str) { if (c == '\\') i++; else if (c == '"') str = false; continue; }
      if (c == '"') str = true;
      else if (c == '{' || c == '[') dep++;
      else if ((c == '}' || c == ']') && --dep == 0) return i + 1;
    }
    return i;
  }
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
  return i;
}

// One rollout line that is a model-visible item. libvrs: `file` is sess - 1, `call`/`pos` of the call tables became cid + nxt, and
// the fields only the old non-synapse windows used (env, tg) are gone.
struct Item {
  struct Part { uint64_t subj = 0; uint8_t mode = 3; std::vector<uint64_t> keys; uint8_t unit = 0; };  // per content item s, mu, kappa (L1 v16 D7′)
  uint32_t len = 0, sess = 0, tok = 0, pos = 0, ord = 0;
  uint64_t off = 0, cid = 0, ch = 0;
  std::atomic<uint64_t> th{0}, rs{0};  // th = θ* of the line, rs = θ* of the first line of its injection bundle: both are functions of the line (written by whoever needs them first, equal values)
  uint8_t kind = 0, mode = 3;
  bool fwd = false;
  std::atomic<uint32_t> nxt{NONE32};  // next item of the same call group of the same session (the group is a stack: the newest item first)
  std::vector<Part> parts;
  std::string mem;                    // memmode: the line itself
};

// content c(m) of an injected item: hash of all its text values
uint64_t content_hash(const std::string& line) {
  std::string all;
  for (size_t at = line.find("\"text\":\""); at != std::string::npos; at = line.find("\"text\":\"", at + 8)) { all += json_string(line, at + 8); all += '\n'; }
  return fnv(all.data(), all.size());
}
// L1 v16 D7′ / §57 / §62: subject, form and keys of each content item of an injected message
enum : uint8_t { MD_FULL = 0, MD_INC = 1, MD_EVENT = 2, MD_UNK = 3, MD_MEMO = 4 };  // MD_MEMO: item aggregate only
// A7′ (§62): the kind of a content item whose kind was not recorded, from the fixed wrapper its renderer wrote; "" = ⊥
std::string wrap_kind(const std::string& t0) {
  size_t a = 0; while (a < t0.size() && (t0[a] == ' ' || t0[a] == '\n' || t0[a] == '\r' || t0[a] == '\t')) a++;
  const std::string_view t(t0.data() + a, t0.size() - a);
  auto st = [&](const char* h) { return t.substr(0, strlen(h)) == h; };
  if (st("<codex_internal_context source=\"goal\">")) return "goal.internal_context";
  if (st("<permissions instructions>")) return "permissions.instructions";
  if (st("<skills_instructions>"))  // §76 E1-56 / §82: shared wrapper; the body decides when only the host locator "(file:" is in it
    return t.find("(file:") != std::string_view::npos && t.find("(executor package:") == std::string_view::npos &&
           t.find("(cloud package:") == std::string_view::npos ? "host_skills.instructions" : "";
  if (st("<model_switch>")) return "model_switch.instructions";
  if (st("# AGENTS.md instructions")) return "agents_md.instructions";
  if (st("<collaboration_mode>")) return t.find("## Heartbeats") != std::string_view::npos ? "" : "collaboration_mode.instructions";  // §76: the app's Heartbeats text: source unconfirmed
  if (st("<apps_instructions>")) return "apps.instructions";
  if (st("<plugins_instructions>")) return "plugins.usage_instructions";
  if (st("<recommended_plugins>")) return "plugins.recommendations";
  if (st("<multi_agent_mode>")) return "multi_agent.mode_instructions";
  if (st("<personality_spec>")) return "personality.spec_instructions";
  if (st("<environment_context>")) return "environments.environment_context";
  if (st("<heartbeat>")) return "user.heartbeat";
  if (st("<subagent_notification>")) return "multi_agent.subagent_notification";
  if (st("<turn_aborted>")) return "generic.turn_aborted";
  if (st("<image_resize_notice>")) return "images.resize_notice";
  if (st("Capabilities from the `")) return "plugins.instructions";
  if (st("<codex_delegation>")) return "client.codex_delegation";
  if (st("Generated images are saved to")) return "client.generated_images";
  if (st("<task-notification>")) return "client.task_notification";
  if (st("<in-app-browser-context")) return "client.in_app_browser_context";  // §64: ambient UI state snapshot (ambiguous persistent: complete)
  if (st("<app-context>")) return "client.app_context";                      // §80: one producer, assembled whole without the previous state
  if (st("You are `/root`, the primary agent")) return "multi_agent.role_instructions";
  if (st("## Memory") && t.find("You have access to a memory folder") != std::string_view::npos && t.find("You have access to a memory folder") < 40) return "memories.instructions";  // §64: same head as the kind era
  return "";
}
// κ(m) of an environment_context item: the elements right under the wrapper (an environment by its id)
void env_keys(const std::string& t, std::vector<uint64_t>& out) {
  size_t i = t.find("<environment_context>"); if (i == std::string::npos) return;
  i += 21; int dep = 0; bool inenvs = false;
  while ((i = t.find('<', i)) != std::string::npos) {
    size_t e = t.find('>', i); if (e == std::string::npos) break;
    const bool close = t[i + 1] == '/', self = t[e - 1] == '/';
    size_t ne = i + 1 + close; while (ne < e && t[ne] != ' ' && t[ne] != '/' && t[ne] != '>') ne++;
    const std::string name = t.substr(i + 1 + close, ne - i - 1 - close);
    if (close) { if (name == "environment_context") break; dep--; if (dep == 0) inenvs = false; }
    else {
      if (dep == 0 && name != "environments") { const std::string k = "e:" + name; out.push_back(fnv(k.data(), k.size())); }
      if (dep == 0 && name == "environments") inenvs = true;
      if (dep == 1 && inenvs && name == "environment") {
        std::string id; if (size_t q = t.find("id=\"", i); q != std::string::npos && q < e) id = t.substr(q + 4, t.find('"', q + 4) - q - 4);
        const std::string k = "env:" + id; out.push_back(fnv(k.data(), k.size()));
      }
      if (!self) dep++;
    }
    i = e + 1;
  }
}
void dash_keys(const std::string& t, std::vector<uint64_t>& out, bool ns = false) {  // one key per "- " line; ns: "- name: description" -> name (tools.rs)
  for (size_t i = 0; i < t.size();) {
    size_t e = t.find('\n', i); if (e == std::string::npos) e = t.size();
    if (e - i > 2 && t[i] == '-' && t[i + 1] == ' ') {
      size_t ke = e;
      if (ns) { const size_t c = t.find(": ", i + 2); if (c != std::string::npos && c < e) ke = c; }
      out.push_back(fnv(t.data() + i + 2, ke - i - 2));
    }
    i = e + 1;
  }
}
// Fixed classification vocabularies have process lifetime without heap storage or
// exit destructors: restore workers may still read them during process shutdown (VI-33).
template <size_t N>
bool part_kind_in(const std::string_view (&kinds)[N], std::string_view kind) {
  return std::find(std::begin(kinds), std::end(kinds), kind) != std::end(kinds);
}

void part_of(const std::string& k0, const std::string& text, Item::Part& P) {
  std::string k = k0;
  if (k.empty() || k == "user.text") k = wrap_kind(text);
  P = Item::Part{};
  if (k.empty()) return;  // ⊥
  std::string key = k;
  static constexpr std::string_view full[] = {
    "goal.internal_context", "host_skills.instructions", "permissions.instructions", "multi_agent.mode_instructions",
    "multi_agent.role_instructions", "multi_agent.usage_hint", "multi_agent.model_catalog", "plugins.recommendations",
    "plugins.usage_instructions", "collaboration_mode.instructions", "memories.instructions", "apps.instructions",
    "generic.developer_instructions", "agents_md.instructions", "model_switch.instructions", "managed_config.developer_instructions",
    "persistent_mode.instructions", "environments.instructions", "skills.catalog", "token_budget.context_window",
    "token_budget.context_window_guidance", "token_budget.reminder", "personality.spec_instructions",
    "client.in_app_browser_context", "client.app_context"};
  static constexpr std::string_view event[] = {
    "generic.turn_aborted", "token_budget.remaining_tokens",  // §74/§76: a status notice without a standing directive (format evidence)
    "rollout_budget.remaining_tokens", "user.image", "user.heartbeat", "client.generated_images", "client.task_notification"};
  uint8_t mode = MD_UNK;
  if (part_kind_in(full, k) || k.rfind("additional_content.", 0) == 0) mode = MD_FULL;
  else if (k == "realtime_conversation.start_instructions" || k == "realtime_conversation.custom_start_instructions" ||
           k == "realtime_conversation.end_instructions") { mode = MD_FULL; key = "realtime_conversation"; }
  else if (k == "skills.selected_skill_instructions") {  // key = skill name
    mode = MD_FULL;
    if (const size_t q = text.find("<name>"); q != std::string::npos) key += "\x1f" + text.substr(q + 6, text.find("</name>", q) - q - 6);
  } else if (k == "tools.deferred_namespaces") {  // E1-58/59: Added/Removed lines are a change (keys = ns); a full render (omission line or not) is full
    if (text.find("Added deferred tool namespaces:") != std::string::npos || text.find("Removed deferred tool namespaces:") != std::string::npos) {
      mode = MD_INC; dash_keys(text, P.keys, true);
    } else mode = MD_FULL;
  } else if (k == "environments.environment_context") { mode = MD_INC; env_keys(text, P.keys); }
  else if (k == "permissions.approved_command_prefix_saved") { mode = MD_INC; key = "permissions.instructions"; dash_keys(text, P.keys); }
  else if (part_kind_in(event, k) || k.rfind("images.", 0) == 0) mode = MD_EVENT;
  // L1 v24 D6¹² / L2 v25 E2-61 (R6-01): a render ends earlier renders of its subject only when it is complete (its renderer
  // dropped nothing: no loss path, or a marked loss path and no mark) or carries the source's own end notice. Otherwise it is
  // partial: its units are the items it lists whole (list renderers) or its lines (other text), and an earlier unit ends only
  // when a later render writes it again. Evidence: content_render_loss_r1.md, content_trunc_markers_r1.md (v140, callers too).
  if (mode == MD_FULL) {
    auto has = [&](const char* m) { return text.find(m) != std::string::npos; };
    static const char* TT_MARK = "tokens truncated\xE2\x80\xA6";
    auto items = [&](bool ns, bool skip_cut) {  // "- " lines; skip_cut: a line whose value was cut ("...") restates nothing
      mode = MD_INC; P.keys.clear(); P.unit = uint8_t(1 + (ns ? 1 : 0) + (skip_cut ? 1 : 0));
      for (size_t i = 0; i < text.size();) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        if (e - i > 2 && text[i] == '-' && text[i + 1] == ' ' && !(skip_cut && e - i >= 3 && text.compare(e - 3, 3, "...") == 0)) {
          size_t ke = e;
          if (ns) { const size_t c = text.find(": ", i + 2); if (c != std::string::npos && c < e) ke = c; }
          P.keys.push_back(fnv(text.data() + i + 2, ke - i - 2));
        }
        i = e + 1;
      }
    };
    auto lines = [&]() {  // every non-empty line written whole; a line holding a cut mark is not a unit
      mode = MD_INC; P.keys.clear(); P.unit = 4;
      for (size_t i = 0; i < text.size();) {
        size_t e = text.find('\n', i); if (e == std::string::npos) e = text.size();
        size_t a = i, z = e; while (a < z && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) a++; while (z > a && (text[z - 1] == ' ' || text[z - 1] == '\t' || text[z - 1] == '\r')) z--;
        if (z > a && std::string_view(text.data() + a, z - a).find(TT_MARK) == std::string_view::npos) P.keys.push_back(fnv(text.data() + a, z - a));
        i = e + 1;
      }
    };
    static constexpr std::string_view lossless[] = {
      "collaboration_mode.instructions", "goal.internal_context", "apps.instructions", "plugins.usage_instructions",
      "environments.instructions", "generic.developer_instructions", "managed_config.developer_instructions",
      "persistent_mode.instructions", "token_budget.context_window", "multi_agent.usage_hint", "model_switch.instructions"};
    const bool recorded = !k0.empty() && k0 != "user.text";
    if (part_kind_in(lossless, k)) {
    } else if (k == "permissions.instructions") {  // H marks the shared path budget (permissions_instructions.rs:40-41); A/B/C have no budget
      if (has("Additional permission paths/globs are omitted") || has("[Some commands were truncated]")) {
        items(false, false);
        for (size_t a = text.find('`'); a != std::string::npos;) {  // writable roots are `path` inside a sentence (:366-380)
          const size_t b = text.find('`', a + 1); if (b == std::string::npos) break;
          if (b > a + 1) P.keys.push_back(fnv(text.data() + a + 1, b - a - 1));
          a = text.find('`', b + 1);
        }
      }
    } else if (k == "memories.instructions") {  // middle cut with a count mark (v140 2,500 tokens)
      if (has(TT_MARK)) lines();
    } else if (k.rfind("additional_content.", 0) == 0) {  // hook output: three marks (content_trunc_markers_r1.md)
      if (has(TT_MARK) || has("Full hook output saved to:") || has("Total output lines:")) lines();
    } else if (k == "tools.deferred_namespaces") {  // H marks (tools.rs:205, "..."); older renderers omit silently
      if (!recorded || has(" additional namespaces omitted.") || has("...\n")) items(true, true);
    } else if (k == "agents_md.instructions") {  // unmarked byte cut; the source's own notices end the earlier text (E2)
      if (!has("These AGENTS.md instructions replace all previously provided AGENTS.md instructions.") &&
          !has("The previously provided AGENTS.md instructions no longer apply.")) lines();
    } else if (k == "host_skills.instructions" || k == "skills.catalog" || k == "multi_agent.model_catalog" || k == "plugins.recommendations") {
      items(true, false);  // silent drops by budget or count (render.rs:912-946, model_catalog.rs:75-78, take(50))
    } else if (k == "multi_agent.mode_instructions") {
      items(false, false);  // the subagents list: 8 lines / 1024 B, silent
    } else lines();  // loss path not ruled out: partial
    if (mode == MD_INC && P.keys.empty()) lines();
  }
  if (mode == MD_INC && P.keys.empty()) { mode = MD_UNK; P.unit = 0; }  // D7′: a change whose keys cannot be read is unknown
  if (mode == MD_UNK) return;  // unknown form: kept, never replaced
  P.subj = fnv(key.data(), key.size()) | 1;
  P.mode = mode;
}
void parts_of(const std::string& line, Item& m) {
  m.parts.clear();
  std::vector<std::string> kinds;
  if (size_t a = line.find("\"content_item_kinds\":["); a != std::string::npos) {
    size_t i = a + 22;
    while (i < line.size() && line[i] == '"') { kinds.push_back(json_string(line, i + 1)); i = json_skip(line, i); if (i < line.size() && line[i] == ',') i++; }
  }
  size_t c = line.find("\"content\":[");
  if (c != std::string::npos) {
    size_t i = c + 11, n = 0;
    while (i < line.size() && line[i] != ']') {
      while (i < line.size() && (line[i] == ',' || line[i] == ' ')) i++;
      if (i >= line.size() || line[i] != '{') break;
      const size_t e = json_skip(line, i);
      std::string text;
      const size_t tx = line.find("\"text\":\"", i);
      if (tx != std::string::npos && tx < e) text = json_string(line, tx + 8);
      Item::Part P; part_of(n < kinds.size() ? kinds[n] : std::string(), text, P);
      m.parts.push_back(std::move(P));
      n++; i = e;
    }
  }
  bool ev = !m.parts.empty(), known = false;
  for (auto& P : m.parts) { ev = ev && P.mode == MD_EVENT; known = known || (P.subj != 0 && P.mode != MD_EVENT); }
  m.mode = ev ? MD_EVENT : known ? MD_UNK : MD_MEMO;
}

// L2 v16 E2-35: owner state per subject = (c_s: theta, rho_s), per key (c_k: theta, rho_k) for the keys after c_s, and the candidate messages.
// A message stays while one of its parts stays (L10″); ⊥ and unknown parts are never replaced. rho only grows: the final state is a
// function of the message set (max merges and the drops they imply), whatever the order of the puts.
// libvrs: the item table is a template parameter (the table is no longer a std::vector).
struct InstrState {
  struct KS { uint64_t th = 0, rho = 0; };
  struct Subj { bool has = false; uint64_t cmax = 0, rho = 0; std::unordered_map<uint64_t, KS> key; std::vector<uint32_t> items; };
  std::unordered_map<uint64_t, Subj> subj;
  std::vector<uint32_t> unk;
  std::vector<uint32_t> run;                      // the drawing run waiting for its close (world_state record or any other record)
  static bool part_alive(uint64_t th, const Item::Part& p, const Subj& q) {
    if (p.mode == MD_FULL) return !q.has || th >= q.rho;
    for (uint64_t k : p.keys) {  // L10″: key k still stands in m iff θ(m) >= ρ_k, c_k = the larger of c_s and the key's holder
      const auto e = q.key.find(k);
      const bool kh = e != q.key.end() && (!q.has || e->second.th > q.cmax);
      if (kh ? th >= e->second.rho : (!q.has || th >= q.rho)) return true;
    }
    return false;
  }
  template <class IT> static bool alive_in(const IT& it, uint32_t m, uint64_t s, const Subj& q) {
    for (const auto& p : it[m].parts) if (p.subj == s && (p.mode == MD_FULL || p.mode == MD_INC) && part_alive(it[m].th, p, q)) return true;
    return false;
  }
  static void merge(Subj& q, uint64_t th, uint64_t rs, const Item::Part& p) {  // max merge of c_s / c_k
    if (p.mode == MD_FULL && (!q.has || th > q.cmax)) {
      q.has = true; q.cmax = th; q.rho = rs;
      for (auto e = q.key.begin(); e != q.key.end();) if (e->second.th < q.cmax) e = q.key.erase(e); else ++e;  // c_s now dominates
    }
    if (p.mode == MD_INC) for (uint64_t k : p.keys) {
      if (q.has && th < q.cmax) continue;
      auto ins = q.key.emplace(k, KS{th, rs});
      if (!ins.second && th > ins.first->second.th) ins.first->second = KS{th, rs};
    }
  }
  template <class IT> void put(const IT& it, uint32_t m) {
    const Item& x = it[m];
    bool u = false;
    for (const auto& p : x.parts) {
      if (p.mode == MD_EVENT) continue;
      if (p.subj == 0 || p.mode == MD_UNK) { u = true; continue; }  // L1 v21 D6¹¹: ⊥/unknown parts are never replaced (R5-01)
      Subj& q = subj[p.subj];
      merge(q, x.th, x.rs, p);
      if (q.items.empty() || q.items.back() != m) q.items.push_back(m);
      q.items.erase(std::remove_if(q.items.begin(), q.items.end(), [&](uint32_t z) { return !alive_in(it, z, p.subj, q); }), q.items.end());
    }
    if (u) unk.push_back(m);
  }
  void inj(uint32_t id) { run.push_back(id); }
  template <class IT> void close(const IT& it) { for (uint32_t m : run) put(it, m); run.clear(); }
  // I(t): the kept messages with θ <= t (the open run included), equal contents once (largest θ)
  template <class IT> void current(const IT& it, uint64_t t, std::vector<uint32_t>& out) const {
    if (!run.empty()) { InstrState c = *this; c.close(it); c.current(it, t, out); return; }
    std::unordered_map<uint64_t, uint32_t> byc;
    auto add = [&](uint32_t m) {
      if (it[m].th > t) return;
      auto ins = byc.emplace(it[m].ch, m);
      if (!ins.second && it[m].th > it[ins.first->second].th) ins.first->second = m;
    };
    for (auto& kv : subj) for (uint32_t m : kv.second.items) add(m);
    for (uint32_t m : unk) add(m);
    for (auto& kv : byc) out.push_back(kv.second);
  }
};

// 3판 2판 정정: a relay = a role user message whose first text starts with "["
bool is_fwd(const std::string& line) {
  const size_t c = line.find("\"content\":[");
  if (c == std::string::npos) return false;
  const size_t tx = line.find("\"text\":\"", c);
  return tx != std::string::npos && tx + 8 < line.size() && line[tx + 8] == '[';
}
// L1 v24 D15/D16, L2 v25 E2-59: s(m) = ceil(e(kappa(m)) / 4), codex estimate_response_item_model_visible_bytes of the item as
// ContextManager::process_item records it (history.rs:338-360, 467-530; utils/string truncate.rs; output-truncation lib.rs)
size_t g_tpol = 12000;                      // tokens: the model's truncation_policy x 1.2 (models cache: tokens 10000)
std::atomic<uint64_t> g_img_orig{0}, g_img_n{0}, g_cut_n{0};  // measurement only: images (detail original), cut tool outputs
size_t esc_len(const char* p, size_t n) {   // serde_json escaped length
  size_t e = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = uint8_t(p[i]);
    e += (c == '"' || c == '\\' || c == '\n' || c == '\t' || c == '\r' || c == 0x08 || c == 0x0c) ? 2 : c < 0x20 ? 6 : 1;
  }
  return e;
}
size_t trunc_esc(const std::string& t, size_t mb) {  // escaped length of truncate_middle_with_token_budget(t, mb / 4)
  if (t.size() <= mb) return esc_len(t.data(), t.size());
  const size_t lb = mb / 2, rb = mb - lb, len = t.size(), tail = len - rb;
  size_t pe = 0, ss = len; bool st = false;
  for (size_t i = 0; i < len;) {
    const uint8_t c = uint8_t(t[i]);
    const size_t cl = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    const size_t ce = std::min(len, i + cl);
    if (ce <= lb) pe = ce;
    else if (i >= tail && !st) { ss = i; st = true; }
    i = ce;
  }
  if (ss < pe) ss = pe;
  const std::string mk = "\xE2\x80\xA6" + std::to_string((len - mb + 3) / 4) + " tokens truncated\xE2\x80\xA6";
  return esc_len(t.data(), pe) + mk.size() + esc_len(t.data() + ss, len - ss);
}
// a data:image/...;base64, URL string starting at q (just past its opening quote): base64 payload length, or 0
size_t img_payload(const std::string& s, size_t q, size_t qe) {
  if (qe < q + 5 || strncasecmp(s.data() + q, "data:", 5) != 0) return 0;
  const size_t cm = s.find(',', q);
  if (cm == std::string::npos || cm >= qe) return 0;
  const std::string meta = s.substr(q + 5, cm - q - 5);
  if (meta.size() < 6 || strncasecmp(meta.data(), "image/", 6) != 0) return 0;
  bool b64 = false;
  for (size_t a = 0; a <= meta.size();) { size_t e = meta.find(';', a); if (e == std::string::npos) e = meta.size();
    if (a > 0 && e - a == 6 && strncasecmp(meta.data() + a, "base64", 6) == 0) b64 = true; a = e + 1; }
  return b64 ? qe - cm - 1 : 0;
}
uint32_t codex_tokens(const std::string& s) {
  const size_t pk = s.find("\"payload\":");
  if (pk == std::string::npos || pk + 10 >= s.size() || s[pk + 10] != '{') return (uint32_t)((s.size() + 3) / 4);
  const size_t b = pk + 10, e = json_skip(s, b);
  int64_t bytes = int64_t(e - b);
  std::string ty;
  if (const size_t tq = s.find("\"type\":\"", b); tq != std::string::npos && tq < e) ty = json_string(s, tq + 8);
  auto done = [&](int64_t by) { return (uint32_t)std::min<int64_t>(UINT32_MAX, (std::max<int64_t>(by, 0) + 3) / 4); };
  if (ty == "reasoning" || ty == "compaction" || ty == "context_compaction") {
    const size_t ek = s.find("\"encrypted_content\":\"", b);
    if (ek != std::string::npos && ek < e) {
      const size_t q = ek + 21, qe = json_skip(s, q - 1) - 1;
      const int64_t n = int64_t(qe - q);
      return done(std::max<int64_t>(0, n * 3 / 4 - 650));
    }
    return done(bytes);
  }
  const size_t mb = 4 * g_tpol;
  auto images = [&](size_t from, size_t to) {  // inline image data URLs inside [from, to)
    for (size_t a = s.find("\"image_url\":\"", from); a != std::string::npos && a < to; a = s.find("\"image_url\":\"", a + 1)) {
      const size_t q = a + 13, qe = json_skip(s, q - 1) - 1;
      const size_t pl = img_payload(s, q, qe);
      if (!pl) continue;
      const size_t oe = s.find('}', qe);
      const bool orig = oe != std::string::npos && s.substr(qe, oe - qe).find("\"detail\":\"original\"") != std::string::npos;
      bytes += 7373 - int64_t(pl); g_img_n.fetch_add(1); if (orig) g_img_orig.fetch_add(1);  // original: codex patch estimate, else 7373
    }
  };
  if (ty == "message") { images(b, e); return done(bytes); }
  if (ty != "function_call_output" && ty != "custom_tool_call_output") return done(bytes);
  const size_t ok = s.find("\"output\":", b);
  if (ok == std::string::npos || ok >= e) return done(bytes);
  size_t v = ok + 9; while (v < e && s[v] == ' ') v++;
  if (s[v] == '"') {  // Text body: truncate_text(content, policy)
    const size_t ve = json_skip(s, v);
    const std::string t = json_string(s, v + 1);
    if (t.size() > mb) { bytes += int64_t(trunc_esc(t, mb)) - int64_t(ve - v - 2); g_cut_n.fetch_add(1); }
    return done(bytes);
  }
  if (s[v] != '[') return done(bytes);
  const size_t ae = json_skip(s, v);
  images(v, ae);
  size_t rem = g_tpol, omitted = 0; bool cut = false;  // truncate_function_output_items_with_policy: remaining budget over the text items
  for (size_t i = v + 1; i < ae;) {
    while (i < ae && (s[i] == ',' || s[i] == ' ')) i++;
    if (i >= ae || s[i] != '{') break;
    const size_t oe = json_skip(s, i);
    std::string it;
    if (const size_t tq = s.find("\"type\":\"", i); tq != std::string::npos && tq < oe) it = json_string(s, tq + 8);
    if (it == "input_text") {
      const size_t xk = s.find("\"text\":\"", i);
      if (xk != std::string::npos && xk < oe) {
        const size_t q = xk + 8, qe = json_skip(s, q - 1);
        const std::string t = json_string(s, q);
        if (rem == 0) { omitted++; bytes -= int64_t(oe - i) + 1; }
        else {
          const size_t cost = (t.size() + 3) / 4;
          if (cost <= rem) rem -= cost;
          else { bytes += int64_t(trunc_esc(t, 4 * rem)) - int64_t(qe - q - 1); rem = 0; cut = true; }
        }
      }
    } else if (it == "encrypted_content") {
      const size_t ek = s.find("\"encrypted_content\":\"", i + 1);
      if (ek != std::string::npos && ek < oe) { const size_t q = ek + 21, qe = json_skip(s, q - 1) - 1; const int64_t n = int64_t(qe - q); bytes += (n * 9 + 15) / 16 - n; }
    }
    i = oe;
  }
  if (omitted) bytes += int64_t(std::string(",{\"type\":\"input_text\",\"text\":\"[omitted " + std::to_string(omitted) + " text items ...]\"}").size());
  if (cut || omitted) g_cut_n.fetch_add(1);
  return done(bytes);
}

uint8_t kind_of(const std::string& line) {
  std::string pt = sval(line, "type", line.find("\"payload\""));
  if (pt == "message") {
    std::string role = sval(line, "role");
    if (role == "assistant") return K_ASSIST;
    if (role != "user") return K_DEV;
    // user utterance vs injected context (L1 정의 2판): codex sends app context, AGENTS.md,
    // environment, heartbeats, notifications as role "user". The first text decides:
    // starting with "<tag" (tag other than image), "#" or "[" means injected.
    std::string tx = sval(line, "text", line.find("\"content\""));
    size_t a = 0;
    while (a < tx.size() && (tx[a] == ' ' || (tx[a] == '\\' && a + 1 < tx.size() && (tx[a + 1] == 'n' || tx[a + 1] == 'r' || tx[a + 1] == 't')))) a += tx[a] == '\\' ? 2 : 1;
    // synapse L1 v6 = 정확입력 §2 (2판): an utterance is what the user typed or answered, including relays
    // ("[클로드 코드 → 코덱스 …]") and answers to the agent's questions; injected = "<tag" or AGENTS.md
    std::string h = tx.substr(a, 40);
    bool img = h.rfind("<image", 0) == 0 || h.rfind("</image", 0) == 0;
    bool reply = h.rfind("<send_user_message_question_reply", 0) == 0;
    bool inj = !img && !reply && (h.rfind("<", 0) == 0 || h.rfind("# AGENTS.md", 0) == 0);
    if (inj) {  // L1 v18 E1-54: a message whose text still has words after its complete leading blocks is the user's (app state + request)
      const size_t c0 = line.find("\"content\"");
      for (size_t at = line.find("\"text\":\"", c0); at != std::string::npos; at = line.find("\"text\":\"", at + 8)) {
        const std::string t = json_string(line, at + 8);
        size_t i = 0;
        auto ws = [&] { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; };
        for (;;) {
          ws();
          if (i >= t.size()) break;
          if (t[i] == '<') {
            size_t ne = i + 1; while (ne < t.size() && t[ne] != ' ' && t[ne] != '>' && t[ne] != '/' && t[ne] != '\n') ne++;
            const std::string close = "</" + t.substr(i + 1, ne - i - 1) + ">";
            const size_t ce = t.find(close, ne);
            if (ne == i + 1 || ce == std::string::npos) { i = t.size(); break; }  // not a complete block: injected (as before)
            i = ce + close.size(); continue;
          }
          if (t.compare(i, 11, "# AGENTS.md") == 0) { const size_t ce = t.find("</INSTRUCTIONS>", i); if (ce == std::string::npos) { i = t.size(); break; } i = ce + 15; continue; }
          return K_USER;  // words remain
        }
      }
    }
    return inj ? K_DEV : K_USER;
  }
  if (pt == "reasoning") return K_REASON;
  if (pt == "function_call") return K_FC;
  if (pt == "function_call_output") return K_FCO;
  if (pt == "custom_tool_call") return K_CTC;
  if (pt == "custom_tool_call_output") return K_CTCO;
  return K_OTHER;
}

// ---------------- the store (D1) ----------------
// libvrs 5 (VRS_코덱스일체화_v1.md §7, P-11): a file has no owner. A line is made known by whoever hands it in (vrs_insert, the tail read of a file):
// its record is filled, its counts are added to the file's ordinal tree, and then it is announced. The thread that finds the announced lines of a file
// joined to the processed prefix takes them over by one CAS on `front` and processes them (no waiting for anybody: a CAS that fails means somebody else
// took them). What a reader (vrs_assemble, a fork reading its parent) sees of a file is the processed prefix `pub`: the lists are cells written by
// position, the counts of the prefix are kept in the record of its last line.

// ---- what a line leaves for the turn it belongs to (agg_feed of 4th stage, made once when the line is handed in)
enum : uint8_t { TF_LO = 1, TF_CALL = 2, TF_OUT = 4, TF_WORK = 8, TF_ASST = 16, TF_IN = 32, TF_AGENT = 64, TF_FINAL = 128 };
struct TD { uint8_t f = 0; std::string nt, text; };  // nt: the event text of a loose item (N(y)); text: the message text, or the final message of a task_complete line
enum : uint16_t { LF_ITEM = 1, LF_USER = 2, LF_DEV = 4, LF_START = 8, LF_COMPLETE = 16, LF_BAD = 32, LF_META = 64 };
struct LineRec {  // one rollout line of a file; fields before `done` are written before the line is announced, the rest by whoever processes it
  uint64_t off = 0, ts = 0, th = 0;
  uint32_t len = 0, ord = 0, item = 0, pfx = 0, r = 0;
  uint32_t ia = 0, ua = 0, sa = 0;  // items / user items / task_started lines up to and including this one (fields of the processing)
  uint32_t nextl = 0;               // next line in the stack of its turn slot
  uint32_t gap = 0;                 // bytes of a NUL-only hole right before this line (restore)
  uint16_t fl = 0;
  std::atomic<uint8_t> done{0};
  std::atomic<TD*> td{nullptr};
};
// the turn started by the m-th task_started line of a file (m = 1, 2, ...): its end, its start facts, the lines that belong to it
struct TurnSlot {
  std::atomic<uint64_t> E{UINT64_MAX};  // (offset << 1) | isStart of the end line: the smallest task_complete in the slot, or the next start; only goes down
  std::atomic<uint64_t> o0{0}, th0{0};
  std::atomic<uint32_t> ord0{0}, pos0{0};
  std::atomic<uint8_t> start_ready{0}, closed{0};
  std::atomic<uint32_t> lst{0};         // head of the stack of the lines (LineRec ids) of the slot
  std::atomic<uint64_t> tot{0};         // bytes of all lines pushed
};
struct UWEnt { uint64_t off; std::shared_ptr<const std::string> s; };
struct UWSet { std::vector<UWEnt> e; };       // P-3': lines handed to vrs_insert whose bytes are not on disk yet, sorted by offset; immutable once published
struct UWRet { const UWSet* set; UWRet* next; };
struct WNode { uint32_t child; uint64_t need; WNode* next; };  // VI-11: a child session waits for its parent to reach ordinal `need`
struct EarlyNode { uint64_t off; std::string line; EarlyNode* next; };  // a line handed in before the restore of its file has set the prefix up
struct Segs { std::vector<std::array<uint32_t, 4>> v; };  // inherited prefix (session, items, users, turns), oldest first; immutable
struct Snap {  // what vrs_assemble reads of a file: built per call from the processed prefix
  uint64_t lastth = 0;
  uint32_t nsess = 0, nusr = 0, ntl = 0;
  bool open = false; uint32_t pos0 = 0; uint64_t th0 = 0, tq1 = 0;
  std::vector<uint64_t> q1;
  std::vector<uint32_t> ocalls;
  const InstrState* ist = nullptr;
  std::unique_ptr<InstrState> own_ist;
  bool segok = false, lin_pending = false;
};

struct Sess {
  // ---- immutable after creation
  uint32_t sg = 0;
  std::string rel, path;      // path relative to its root's directory, absolute path
  std::string nu;             // VRS_onePC_v1.md D-u1: the name in the store = tag + rel (lookup, shard name, order)
  uint32_t root32 = 0;        // stable lineage id of this file (hash of rel); a cell remembers lineage roots by it
  bool is_base = false;       // the file existed when the library was opened
  bool memmode = false;       // no file behind this session (an ephemeral thread): the lines stay in memory
  // ---- lists: cells written by position, read as written prefixes
  Cells sess, usr, tl;
  OrdTree ot;                 // P-11: counts of items / users / task_started lines by ordinal
  Sparse<TurnSlot> slots;
  // ---- the processed prefix (byte offsets, only move up)
  std::atomic<uint64_t> front{0};  // lines before it are taken over by some thread
  std::atomic<uint64_t> pub{0};    // lines before it are processed
  std::atomic<uint64_t> insert_end{0}; // N-F: max end explicitly supplied by vrs_insert; readiness, not selection
  uint64_t base_end = 0;           // restore: lines before it are in the tables and have no LineRec (set before `restored`)
  struct Base { bool has = false; uint64_t th = 0, ts = 0; uint32_t r = 0, ia = 0, ua = 0, sa = 0, ord = 0; } base;  // the last line before base_end
  uint32_t rturns = 0;             // restored closed turns (their slots are not used)
  std::unordered_set<uint64_t> have_rec;  // restore: offsets of the lines after base_end whose records the shard holds already (the lines of the open turn are read again; their records are not written again)
  std::atomic<uint64_t> pubord{0}; // 1 + the largest ordinal in the processed prefix
  // ---- lineage and instruction state
  std::atomic<const Segs*> segs{nullptr};
  std::atomic<int> seg_state{0};   // 0 = not set up, 1 = being set up, 2 = set up
  std::atomic<int> lin_pending{0}; // VI-11: the parent has not reached the fork ordinal yet
  std::atomic<WNode*> waiters{nullptr};
  std::atomic<uint64_t> wait_key{0};  // the (parent, ordinal) this session registered for
  std::atomic<const InstrState*> ist{nullptr};  // closed injection bundles of the file (copy-on-write, replaced by CAS)
  std::atomic<uint32_t> cfront{0}; // turns before it have been handed to the counting
  // ---- state of the file
  std::atomic<int> restored{1};    // 0 until restore (P-2) has run for a file that existed at open
  std::atomic<EarlyNode*> early{nullptr};
  std::string id, fpar; uint64_t fk = 0;
  std::atomic<int> meta_pub{0};    // id / fpar / fk are set
  std::atomic<int> finished{0};
  std::atomic<int> ing_q{0};       // an ingest task is queued
  std::shared_ptr<VrsHandleKey> read_key = std::make_shared<VrsHandleKey>();
  std::shared_ptr<VrsHandleKey> shard_key = std::make_shared<VrsHandleKey>();
  std::atomic<bool> shard_started{false};  // fact only; no OS handle retained by Sess
  std::atomic<uint64_t> wm_pub{0};            // P-3': the largest confirmed-prefix mark (R_WM) this process has put in the shard
  std::atomic<const UWSet*> uw{nullptr};      // P-3': unwritten lines of this file (null = none); copy-on-write, replaced by CAS
  std::atomic<UWRet*> uw_ret{nullptr};        // retired sets waiting for the readers to let go
  ~Sess() {
    while (WNode* n = waiters.load(std::memory_order_relaxed)) { waiters.store(n->next, std::memory_order_relaxed); delete n; }
    while (EarlyNode* n = early.load(std::memory_order_relaxed)) { early.store(n->next, std::memory_order_relaxed); delete n; }
    delete uw.load(std::memory_order_relaxed);
    while (UWRet* n = uw_ret.load(std::memory_order_relaxed)) { uw_ret.store(n->next, std::memory_order_relaxed); delete n->set; delete n; }
    delete ist.load(std::memory_order_relaxed);
    delete segs.load(std::memory_order_relaxed);
  }
};

VrsFdLease source_fd(Sess& ss) {
  if (ss.memmode) return {};
  return VrsHandleCache<VrsFd>::local().get(ss.read_key, [&]() -> VrsFdLease {
    auto file = std::make_shared<VrsFd>(vp_open_ro(ss.path.c_str()));
    if (file->fd < 0 || !ss.read_key->accepts(file->fd)) return {};
    return file;
  });
}

struct SessList {  // S.sess[sg], S.usr[sg], S.tl[sg] as in 42f
  const std::atomic<Sess*>* sp; int w;
  const Cells& operator[](uint32_t sg) const { Sess& s = *sp[sg].load(std::memory_order_acquire); return w == 0 ? s.sess : w == 1 ? s.usr : s.tl; }
};
struct TTab {  // the turn table with the 42f syntax TT[x]->field
  GVec<TurnRec, 10, 24> v;
  TurnRec* operator[](size_t i) const { return &v[i]; }
};

// N-B: unordered work obligations; implementation and publication proof: vrs_workset.h.
// restore file i / the first look at a session after the load (lineage, restored contributions, counting) / count the closed turns of a session /
// read the new bytes of a file / the lineage of a session can be tried again / check a batch of (turn, observer) pairs after a turn came into the order (P-10)
enum : uint32_t { T_RESTORE = 0, T_FIRST = 1, T_COUNT = 2, T_INGEST = 3, T_KICK = 4, T_REVISIT = 5, T_K5 = 6 };  // T_K5: a task of the K5 layer
inline uint64_t task_of(uint32_t kind, uint64_t id) { return (uint64_t(kind) << 56) | id; }

constexpr int HP_N = 512;
struct Store;
uint64_t ident_hash(const Sess& ss);
void bump_progress(Store& S);
// ---- D3′ 𝔅 (VI-08): every closed turn of the files in the store, ordered by key = (theta_e, path of the rollout, ord0). Buckets of DS_TH
// (one sorted immutable array each, replaced by copy + CAS) and a Fenwick tree of the bucket counts (atomics). Nothing is frozen: a new turn
// is put in when it closes (or is restored), rank and select are O(log days + log bucket). Old arrays are retired and freed when no
// reader holds them (hazard slots), with no lock.
constexpr uint64_t DS_TH = (1ull << 16) * TH_SEC;  // D_S: the width of a bucket (42f line 57)
struct BEnt { uint64_t the; uint32_t id; uint32_t pad; uint64_t h; };  // h = identity hash of the turn (fingerprints)
struct Bkt {
  uint32_t n; uint32_t pad;
  BEnt* e() { return reinterpret_cast<BEnt*>(this + 1); }
  const BEnt* e() const { return reinterpret_cast<const BEnt*>(this + 1); }
};
constexpr uint32_t BDAYS = 1u << 17;
struct BRet { Bkt* b; BRet* next; };
struct BOrder {
  Store* st = nullptr;
  std::atomic<uint64_t>* yields = nullptr;  // counts the turns of every yield loop of the library (Store::insert_waits)
  std::unique_ptr<std::atomic<Bkt*>[]> bk{new std::atomic<Bkt*>[BDAYS]()};
  std::unique_ptr<std::atomic<int32_t>[]> fw{new std::atomic<int32_t>[BDAYS + 1]()};
  std::unique_ptr<std::atomic<uint64_t>[]> fh{new std::atomic<uint64_t>[BDAYS + 1]()};  // the same Fenwick tree over the identity hashes (wrapping sums)
  std::atomic<uint64_t> total{0}, miss{0};
  std::atomic<uint64_t> started{0}, finished{0};  // inserts begun / inserts whose bucket and Fenwick trees are all done: a reader that sees both the same before and after its work read one state of 𝔅
  std::atomic<const Bkt*> hz[HP_N]{};
  std::atomic<int> hz_used[HP_N]{};
  std::atomic<BRet*> ret{nullptr};
  std::atomic<int> nret{0};
  static uint32_t day(uint64_t the) { const uint64_t d = the / DS_TH; return d >= BDAYS ? BDAYS - 1 : (uint32_t)d; }
  static Bkt* mk(uint32_t n) { Bkt* b = static_cast<Bkt*>(std::malloc(sizeof(Bkt) + (size_t)n * sizeof(BEnt))); b->n = n; b->pad = 0; return b; }
  int slot() {
    const unsigned start = (unsigned)(std::hash<std::thread::id>()(std::this_thread::get_id()) % HP_N);
    for (;;) {
      for (unsigned k = 0; k < HP_N; k++) { const unsigned i = (start + k) % HP_N; int z = 0; if (hz_used[i].compare_exchange_strong(z, 1)) return (int)i; }
      if (yields) yields->fetch_add(1, std::memory_order_relaxed);
      std::this_thread::yield();
    }
  }
  const Bkt* protect(uint32_t d, int sl) {
    const Bkt* b;
    do { b = bk[d].load(std::memory_order_acquire); hz[sl].store(b, std::memory_order_seq_cst); } while (b != bk[d].load(std::memory_order_seq_cst));
    return b;
  }
  void release(int sl) { hz[sl].store(nullptr, std::memory_order_release); hz_used[sl].store(0, std::memory_order_release); }
  bool ent_less(const BEnt& a, const BEnt& b) const;  // key order (needs the turn table and the paths)
  void insert(uint32_t id, uint64_t the);
  uint64_t rank(uint64_t the);        // |{r : theta_e(r) < the}|
  void fp(uint64_t the, uint64_t& rank_out, uint64_t& hsum_out);  // F(the) = (|{r : theta_e(r) < the}|, sum of the identity hashes of those turns)
  uint32_t select(uint64_t p);        // the turn at position p in key order
  uint64_t hpos(uint64_t p);          // sum of the identity hashes of the first p turns in key order (P-9: the fingerprint of a window is the difference of two of these)
  uint32_t operator[](size_t p) { return select(p); }
  void retire(Bkt* b) {
    BRet* r = new BRet{b, ret.load(std::memory_order_relaxed)};
    while (!ret.compare_exchange_weak(r->next, r, std::memory_order_release, std::memory_order_relaxed)) {}
    if (nret.fetch_add(1) >= 32) reclaim();
  }
  void reclaim() {
    BRet* list = ret.exchange(nullptr, std::memory_order_acq_rel);
    if (!list) return;
    std::vector<const Bkt*> hs;
    for (int k = 0; k < HP_N; k++) if (const Bkt* p = hz[k].load(std::memory_order_seq_cst)) hs.push_back(p);
    std::sort(hs.begin(), hs.end());
    int freed = 0;
    while (list) {
      BRet* nx = list->next;
      if (std::binary_search(hs.begin(), hs.end(), (const Bkt*)list->b)) {
        BRet* h = ret.load(std::memory_order_relaxed);
        do { list->next = h; } while (!ret.compare_exchange_weak(h, list, std::memory_order_release, std::memory_order_relaxed));
      } else { std::free(list->b); delete list; freed++; }
      list = nx;
    }
    nret.fetch_sub(freed);
  }
  ~BOrder() {
    for (uint32_t d = 0; d < BDAYS; d++) std::free(bk[d].load(std::memory_order_relaxed));
    while (BRet* r = ret.load(std::memory_order_relaxed)) { ret.store(r->next, std::memory_order_relaxed); std::free(r->b); delete r; }
  }
};
struct RetNode { void* p; uint8_t kind; size_t bytes; RetNode* next; };  // kind 0 ObsY, 1 A0Val, 2 Contrib, 3 InstrState, 4 TD, 5 K5U, 6 K5C
void k5_free_obj(void* p, uint8_t kind);
// Replaced caches and contributions (P-8) are freed by epochs, with no lock: a thread that may hold one of them is inside an EbrGuard (registered under the epoch it
// found); an object retired in epoch e can be held only by threads registered in an epoch <= e; when the threads of epoch e-1 are gone the epoch moves on and the
// objects retired in e-1 are freed. Nobody waits: a collection that finds a thread in the old epoch leaves the objects for the next attempt.
struct Ebr {
  std::atomic<uint64_t> E{0};
  std::atomic<int64_t> cnt[3]{};
  std::atomic<RetNode*> lst[3]{};
};
void free_ret(RetNode* r) {
  if (r->kind >= 5) { k5_free_obj(r->p, r->kind); delete r; return; }
#ifdef VRS_EBR_POISON  // test build: a freed object is overwritten with 0xDD and not given back; a reader that still holds it reads garbage (and crashes or disagrees)
  std::memset(r->p, 0xDD, r->kind == 0 ? sizeof(ObsY) : r->kind == 1 ? sizeof(A0Val) : r->kind == 2 ? sizeof(Contrib) : r->kind == 3 ? sizeof(InstrState) : sizeof(TD));
#else
  if (r->kind == 0) delete static_cast<ObsY*>(r->p); else if (r->kind == 1) delete static_cast<A0Val*>(r->p); else if (r->kind == 2) delete static_cast<Contrib*>(r->p);
  else if (r->kind == 3) delete static_cast<InstrState*>(r->p); else delete static_cast<TD*>(r->p);
#endif
  delete r;
}
struct Root {  // VRS_onePC_v1.md B2: fixed at open
  std::string tag, dir, pre;  // pre = dir + "/"
  std::string change_markers, writer_markers;
  void (*recover)(void*) = nullptr;
  void* recover_context = nullptr;
};
struct K5State;
void k5_free(Store& S);
struct Store {
  K5State* k5 = nullptr;      // K5 3판 (vrs_k5.cpp)
  // ---- options (vrs_opts)
  std::string sdir, idir;     // sdir = roots[0].dir (the Linux watcher, which takes one root)
  std::vector<Root> roots;
  bool segwin = false;
  uint64_t budget = 258400;
  unsigned nworkers = 12;
  std::vector<unsigned> cpus;
  // ---- tables (ids start at 1; cell value 0 = nothing written)
  GVec<Item, 15, 32> it;
  std::atomic<uint32_t> nit{0};
  GVec<LineRec, 15, 32> lr;
  std::atomic<uint32_t> nline{0};
  TTab TT;
  std::atomic<uint32_t> nturn{0};
  std::unique_ptr<std::atomic<Sess*>[]> sp{new std::atomic<Sess*>[65536]()};
  std::atomic<uint32_t> nsg{0};
  SessList sess{sp.get(), 0}, usr{sp.get(), 1}, tl{sp.get(), 2};
  LFMap<std::atomic<uint32_t>> pathmap, idmap;  // rel path -> session, thread id -> session
  LFMap<std::atomic<uint32_t>> lmap, lend;      // (session, offset of a line) -> its record, (session, offset after a line) -> its record
  LFMap<std::atomic<uint64_t>> holes, holes_end;  // (session, start of a NUL-only hole) -> its end; (session, end of a hole) -> its start
  LFMap<PSlot> PT;
  LFMap<CallVal> calls;
  Cell cells[NCA];
  LFMap<std::atomic<uint32_t>> fidmap;          // VI-09: id in the file name -> session (filled when a file is found)
  mutable BOrder B;                             // D3′ 𝔅: all closed turns of the files
  std::atomic<int> loaded{0}, ready{0};         // loaded: every shard is in; ready: also nothing left to count
  std::atomic<int64_t> pend_total{0};            // closed turns without a contribution unit, plus revisit tasks in flight, plus lines handed in and not processed
  bool stale_index = false;                      // the index was built for another spelling of the sessions directory: its shards are not used
  std::atomic<int64_t> fin_left{0};              // base sessions whose first look after the load has not run
  std::atomic<uint32_t> progress{0};             // 6th stage P-14: bumped (and futex-woken) when ready is set or a lineage is set up; vrs_wait sleeps on it
  std::atomic<uint32_t> pub_waiters{0};          // KW3 (VRS_MCP_libvrs_v1.md 14.4): callers in vrs_wait_off; while > 0 a move of a processed prefix bumps progress
  std::chrono::steady_clock::time_point hold_ready{};  // test hook VRS_DBG_HOLD_READY_MS (race injection T6-2): ready is not latched before this
  std::atomic<int> hold_armed{0};
  bool dbg_fail_ready = false;  // test hook VRS_DBG_FAIL_READY (T6-2)
  mutable std::atomic<uint64_t> lemmaE{0};
  // ---- restore (P-2)
  std::vector<Sess*> base;                // sessions of the files that existed at open, in sorted path order
  VrsWorkSet work, first;                    // N-B: ready tasks / initially dependent session checks
  std::atomic<uint64_t> tasks_done{0}, lin_waits{0};
  std::atomic<uint32_t> restore_left{0};   // restore tasks not finished; observed independently by every worker
  std::atomic<int> stop{0};
  std::vector<std::thread> workers;
  std::vector<std::thread> watchers;  // B4: one per root
  int stop_fd = -1;
  std::chrono::steady_clock::time_point t_open;
  std::atomic<int64_t> ready_us{-1};
  // ---- reader slots for the unwritten-line sets (P-3')
  mutable std::atomic<const UWSet*> uwhz[HP_N]{};
  mutable std::atomic<int> uwhz_used[HP_N]{};
  std::atomic<int64_t> uw_n{0}, uw_bytes{0};
  std::atomic<uint64_t> uw_added{0}, uw_released{0}, uw_unknown{0}, holes_skipped{0}, nul_stops{0};
  mutable std::atomic<uint64_t> insert_waits{0};   // turns of every yield loop in the library (a thread that gave up its time slice for another thread); 0 = nobody ever waited
  // ---- P-9 / P-10 / D11: observers by window, units that can be replaced
  LFMap<std::atomic<int64_t>> rootcnt;            // (cell, lineage root) -> number of cell adds of that root that are in the cell (lin1 = roots with a positive count)
  mutable Ebr ebr;                                // replaced observers / A0 sets / units / instruction states / turn data, freed by epochs
  mutable std::atomic<uint64_t> late_events{0}, reval_checked{0}, recounts{0}, count_retries{0}, ctr_n{0}, ctr_bytes{0}, obs_rebuilt{0}, a0_rebuilt{0}, retired_n{0}, retired_bytes{0}, freed_n{0},
      settle_ok{0}, settle_bad{0}, dep_pairs{0}, dep_viol{0}, y_max{0}, x_max{0}, x_total{0}, y_total{0}, bad_lines{0}, runs{0}, run_max{0}, dup_starts{0}, pass_retries{0}, revisit_tasks{0};
  // ---- counters (test hooks, vrs_dbg_stats)
  std::atomic<uint64_t> dbg_checked{0}, dbg_mismatch{0}, dbg_dec{0}, dbg_maxgroup{0}, shards_loaded{0}, shards_rebuilt{0}, shard_corrupt{0}, shard_err{0},
      lines_applied{0}, lines_in{0}, restored_items{0}, tail_lines{0};

  Sess& sx(uint32_t sg) const { return *sp[sg].load(std::memory_order_acquire); }
  ~Store() {
    k5_free(*this);
    for (uint32_t i = 1, e = nturn.load(); i <= e; i++) {
      TurnRec& t = *TT[i]; delete t.obs.load(); delete t.a0.load(); delete t.ctr.load();
      for (Contrib* c = t.cand; c;) { Contrib* nx = c->next; delete c; c = nx; }
      for (DepChunk* c = t.deps.load(); c;) { DepChunk* nx = c->next.load(); delete c; c = nx; }
    }
    for (uint32_t i = 1, e = nline.load(); i <= e; i++) delete lr[i].td.load();
    for (int k = 0; k < 3; k++) for (RetNode* r = ebr.lst[k].load(); r;) { RetNode* nx = r->next; free_ret(r); r = nx; }
    for (uint32_t q = 1, e = nsg.load(); q <= e; q++) delete sp[q].load();
    if (stop_fd >= 0) vp_stop_close(stop_fd);
  }
  const PSlot* pt_find(uint64_t key) const { auto* n = PT.find(key); return n ? &n->val : nullptr; }
  void pt_add(uint32_t sg, uint64_t h, uint32_t tpos) {  // sorted insert; the turns of a session may close in any order
    PSlot& s = PT.get(pt_key(h, sg))->val;
    auto* nd = new PNode; nd->tpos = tpos;
    std::atomic<PNode*>* link = &s.head;
    if (PNode* t = s.tail.load(std::memory_order_acquire); t && t->tpos < tpos) link = &t->next;
    for (;;) {
      PNode* c = link->load(std::memory_order_acquire);
      while (c && c->tpos < tpos) { link = &c->next; c = link->load(std::memory_order_acquire); }
      nd->next.store(c, std::memory_order_relaxed);
      if (link->compare_exchange_strong(c, nd, std::memory_order_release, std::memory_order_acquire)) break;
    }
    if (!nd->next.load(std::memory_order_acquire)) {
      PNode* tt = s.tail.load(std::memory_order_acquire);
      while ((!tt || tt->tpos < tpos) && !s.tail.compare_exchange_weak(tt, nd, std::memory_order_acq_rel)) {}
    }
  }
};

// ---- 𝔅 operations (the key order: theta_e, then the path of the rollout in the order of std::filesystem::path, which is the order in which a
// fresh 42f numbers its sessions, then ord0)
bool BOrder::ent_less(const BEnt& a, const BEnt& b) const {
  if (a.the != b.the) return a.the < b.the;
  const TurnRec& x = *st->TT[a.id]; const TurnRec& y = *st->TT[b.id];
  if (x.sg != y.sg) { const int c = vp_path(st->sx(x.sg).nu).compare(vp_path(st->sx(y.sg).nu)); if (c != 0) return c < 0; }  // B5: the store name
  return x.ord0 < y.ord0;
}
void BOrder::insert(uint32_t id, uint64_t the) {
  const uint32_t d = day(the);
  const BEnt ne{the, id, 0, ci_mix(st->TT[id]->seed)};
  started.fetch_add(1, std::memory_order_seq_cst);
  for (;;) {
    const int sl = slot();
    const Bkt* old = protect(d, sl);
    const uint32_t n = old ? old->n : 0;
    uint32_t lo = 0, hi = n;  // first entry that is not less than the new one
    while (lo < hi) { const uint32_t md = (lo + hi) / 2; if (ent_less(old->e()[md], ne)) lo = md + 1; else hi = md; }
    Bkt* nb = mk(n + 1);
    if (n) { std::memcpy(nb->e(), old->e(), (size_t)lo * sizeof(BEnt)); std::memcpy(nb->e() + lo + 1, old->e() + lo, (size_t)(n - lo) * sizeof(BEnt)); }
    nb->e()[lo] = ne;
    Bkt* ex = const_cast<Bkt*>(old);
    const bool ok = bk[d].compare_exchange_strong(ex, nb, std::memory_order_acq_rel);
    release(sl);
    if (ok) {
      for (uint32_t i = d + 1; i <= BDAYS; i += i & (0u - i)) { fw[i].fetch_add(1, std::memory_order_acq_rel); fh[i].fetch_add(ne.h, std::memory_order_acq_rel); }
      total.fetch_add(1, std::memory_order_relaxed);
      finished.fetch_add(1, std::memory_order_seq_cst);
      if (old) retire(const_cast<Bkt*>(old));
      return;
    }
    std::free(nb);
  }
}
uint64_t BOrder::rank(uint64_t the) {
  const uint32_t d = day(the);
  uint64_t s = 0;
  for (uint32_t i = d; i > 0; i -= i & (0u - i)) s += (uint64_t)fw[i].load(std::memory_order_acquire);  // days before d
  const int sl = slot();
  const Bkt* b = protect(d, sl);
  if (b) {
    uint32_t lo = 0, hi = b->n;
    while (lo < hi) { const uint32_t md = (lo + hi) / 2; if (b->e()[md].the < the) lo = md + 1; else hi = md; }
    s += lo;
  }
  release(sl);
  return s;
}
void BOrder::fp(uint64_t the, uint64_t& rank_out, uint64_t& hsum_out) {
  const uint32_t d = day(the);
  uint64_t s = 0, hs = 0;
  for (uint32_t i = d; i > 0; i -= i & (0u - i)) { s += (uint64_t)fw[i].load(std::memory_order_acquire); hs += fh[i].load(std::memory_order_acquire); }
  const int sl = slot();
  const Bkt* b = protect(d, sl);
  if (b) {
    uint32_t lo = 0, hi = b->n;
    while (lo < hi) { const uint32_t md = (lo + hi) / 2; if (b->e()[md].the < the) lo = md + 1; else hi = md; }
    s += lo;
    for (uint32_t k = 0; k < lo; k++) hs += b->e()[k].h;
  }
  release(sl);
  rank_out = s; hsum_out = hs;
}
uint64_t BOrder::hpos(uint64_t p) {
  uint64_t rem = p, hs = 0; uint32_t pos = 0;
  for (uint32_t step = BDAYS; step; step >>= 1) {
    const uint32_t nx = pos + step;
    if (nx <= BDAYS) { const int64_t c = fw[nx].load(std::memory_order_acquire); if ((uint64_t)c <= rem) { pos = nx; rem -= (uint64_t)c; hs += fh[nx].load(std::memory_order_acquire); } }
  }
  if (rem && pos < BDAYS) {
    const int sl = slot();
    const Bkt* b = protect(pos, sl);
    if (b) for (uint32_t k = 0; k < b->n && k < rem; k++) hs += b->e()[k].h;
    release(sl);
  }
  return hs;
}
uint32_t BOrder::select(uint64_t p) {
  uint64_t rem = p; uint32_t pos = 0;
  for (uint32_t step = BDAYS; step; step >>= 1) {
    const uint32_t nx = pos + step;
    if (nx <= BDAYS) { const int64_t c = fw[nx].load(std::memory_order_acquire); if ((uint64_t)c <= rem) { pos = nx; rem -= (uint64_t)c; } }
  }
  uint32_t out = 0;
  if (pos < BDAYS) {
    const int sl = slot();
    const Bkt* b = protect(pos, sl);
    if (b && b->n) out = b->e()[rem < b->n ? rem : b->n - 1].id; else miss.fetch_add(1, std::memory_order_relaxed);
    release(sl);
  } else miss.fetch_add(1, std::memory_order_relaxed);
  return out;
}

// L2 7판: a session list seen through its inherited segments, without copying ids
struct View {
  std::vector<const Cells*> ptr; std::vector<size_t> off; std::vector<uint32_t> ssg; size_t n = 0;
  void add(uint32_t s, const Cells* p, size_t k) { ssg.push_back(s); ptr.push_back(p); off.push_back(n); n += k; }
  uint32_t operator[](size_t i) const { size_t k = ptr.size() - 1; while (k > 0 && i < off[k]) k--; return (*ptr[k])[i - off[k]]; }
  size_t size() const { return n; }
  size_t idx(uint32_t sess, uint32_t pos) const { for (size_t k = 0; k < ssg.size(); k++) if (ssg[k] == sess) return off[k] + pos; return pos; }
};

// D11: the unit of a turn goes into the cells in place of the one it replaces (null = none). A cell is the sum of the units applied to it; the replacement is
// the difference of the two units in that cell, one add per lane (an = a * 2^32 + n packs two counters, so a difference that is negative in one of them is
// still right as long as both counters stay >= 0, which they do: a unit only takes out what it put in). 42f's lin1 / lin2 only tell none / one / two-or-more
// lineage roots; a root can go away again, so libvrs counts the cell adds per (cell, root) and keeps in `lin1` the number of roots with a positive count
// (the reader tests lin1 == 0 / >= 2).
void apply_unit(Store& S, const Contrib* nw, const Contrib* old) {
  struct D { int idx; uint32_t root; int64_t da, db, dn; };
  std::vector<D> d;
  auto add = [&](const Contrib* c, int sign) {
    if (!c) return;
    for (const CEnt& e : c->e) {
      const bool alpha = (e.fl & 1) != 0, beta = (e.fl & 2) != 0;
      const int64_t n = int64_t(e.cnt) * sign;
      d.push_back(D{e.cell, c->root, alpha ? ((int64_t(1) << 32) + 1) * n : n, alpha != beta ? (alpha ? (int64_t(1) << 32) * n : n) : 0, n});
    }
  };
  add(nw, +1); add(old, -1);
  std::sort(d.begin(), d.end(), [](const D& a, const D& b) { return a.idx != b.idx ? a.idx < b.idx : a.root < b.root; });
  for (size_t i = 0; i < d.size();) {
    size_t j = i; int64_t da = 0, db = 0;
    const int idx = d[i].idx;
    for (; j < d.size() && d[j].idx == idx; j++) { da += d[j].da; db += d[j].db; }
    Cell& cl = S.cells[idx];
    if (da) cl.an.fetch_add((uint64_t)da);
    if (db) cl.ab.fetch_add((uint64_t)db);
    for (size_t a = i; a < j;) {  // the counts per root of this cell
      size_t b = a; int64_t dn = 0;
      for (; b < j && d[b].root == d[a].root; b++) dn += d[b].dn;
      if (dn) {
        auto* nd = S.rootcnt.get(((uint64_t(idx) << 32) | d[a].root) + 1);
        const int64_t prev = nd->val.fetch_add(dn), now = prev + dn;
        if (prev <= 0 && now > 0) cl.lin1.fetch_add(1); else if (prev > 0 && now <= 0) cl.lin1.fetch_sub(1);
      }
      a = b;
    }
    i = j;
  }
}
struct EbrGuard {  // the thread may hold ObsY / A0Val / Contrib pointers while this lives
  const Store& S; uint64_t e;
  explicit EbrGuard(const Store& s) : S(s) {
    for (;;) {
      e = S.ebr.E.load(std::memory_order_seq_cst);
      S.ebr.cnt[e % 3].fetch_add(1, std::memory_order_seq_cst);
      if (S.ebr.E.load(std::memory_order_seq_cst) == e) break;
      S.ebr.cnt[e % 3].fetch_sub(1, std::memory_order_seq_cst);
    }
  }
  ~EbrGuard() { S.ebr.cnt[e % 3].fetch_sub(1, std::memory_order_seq_cst); }
  EbrGuard(const EbrGuard&) = delete;
  EbrGuard& operator=(const EbrGuard&) = delete;
};
void ebr_collect(const Store& S) {  // move the epoch on if nobody is in the one before; free what was retired then
  uint64_t e = S.ebr.E.load(std::memory_order_seq_cst);
  if (S.ebr.cnt[(e + 2) % 3].load(std::memory_order_seq_cst) != 0) return;
  if (!S.ebr.E.compare_exchange_strong(e, e + 1, std::memory_order_seq_cst)) return;
  RetNode* l = S.ebr.lst[(e + 2) % 3].exchange(nullptr, std::memory_order_seq_cst);
  while (l) {
    RetNode* nx = l->next;
    S.retired_bytes.fetch_sub(l->bytes, std::memory_order_relaxed); S.retired_n.fetch_sub(1, std::memory_order_relaxed); S.freed_n.fetch_add(1, std::memory_order_relaxed);
    free_ret(l);
    l = nx;
  }
}
void retire_obj(const Store& S, void* p, uint8_t kind, size_t bytes) {  // unlinked from its owner (the caller holds an EbrGuard)
  const uint64_t e = S.ebr.E.load(std::memory_order_seq_cst);
  RetNode* r = new RetNode{p, kind, bytes, S.ebr.lst[e % 3].load(std::memory_order_relaxed)};
  while (!S.ebr.lst[e % 3].compare_exchange_weak(r->next, r, std::memory_order_release, std::memory_order_relaxed)) {}
  S.retired_n.fetch_add(1, std::memory_order_relaxed); S.retired_bytes.fetch_add(bytes, std::memory_order_relaxed);
  ebr_collect(S);
}

// ---- P-3': the bytes of lines that vrs_insert has but the file may not hold yet. Copy-on-write set per file, replaced by CAS; readers protect the
// set they look at with a hazard slot, a replaced set is freed once no slot holds it. No lock, nobody waits for anybody.
int uw_slot(const Store& S) {
  for (;;) {
    const unsigned start = (unsigned)(std::hash<std::thread::id>()(std::this_thread::get_id()) % HP_N);
    for (unsigned k = 0; k < HP_N; k++) { const unsigned i = (start + k) % HP_N; int z = 0; if (S.uwhz_used[i].compare_exchange_strong(z, 1)) return (int)i; }
    S.insert_waits.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::yield();
  }
}
const UWSet* uw_protect(const Store& S, const Sess& ss, int slot) {
  const UWSet* p;
  do { p = ss.uw.load(std::memory_order_acquire); S.uwhz[slot].store(p, std::memory_order_seq_cst); } while (p != ss.uw.load(std::memory_order_seq_cst));
  return p;
}
void uw_release(const Store& S, int slot) { S.uwhz[slot].store(nullptr, std::memory_order_release); S.uwhz_used[slot].store(0, std::memory_order_release); }
void uw_reclaim(Store& S, Sess& ss) {
  UWRet* l = ss.uw_ret.exchange(nullptr, std::memory_order_acq_rel);  // take the whole list; what is still read goes back
  while (l) {
    UWRet* nx = l->next;
    bool used = false;
    for (int k = 0; k < HP_N; k++) if (S.uwhz[k].load(std::memory_order_seq_cst) == l->set) { used = true; break; }
    if (used) { UWRet* h = ss.uw_ret.load(std::memory_order_relaxed); do { l->next = h; } while (!ss.uw_ret.compare_exchange_weak(h, l, std::memory_order_release, std::memory_order_relaxed)); }
    else { delete l->set; delete l; }
    l = nx;
  }
}
void uw_retire(Store& S, Sess& ss, const UWSet* old) {
  if (old) { UWRet* n = new UWRet{old, nullptr}; UWRet* h = ss.uw_ret.load(std::memory_order_relaxed); do { n->next = h; } while (!ss.uw_ret.compare_exchange_weak(h, n, std::memory_order_release, std::memory_order_relaxed)); }
  if (ss.uw_ret.load(std::memory_order_relaxed)) uw_reclaim(S, ss);
}
// keep the bytes of the line at off until vrs_written; false = the offset was held already
bool uw_add(Store& S, Sess& ss, uint64_t off, const char* line, size_t len) {
  auto str = std::make_shared<const std::string>(line ? line : "", len);
  const int sl = uw_slot(S);
  const UWSet* cur;
  for (;;) {
    cur = uw_protect(S, ss, sl);
    size_t pos = 0, n = cur ? cur->e.size() : 0;
    if (cur) {
      pos = (size_t)(std::lower_bound(cur->e.begin(), cur->e.end(), off, [](const UWEnt& a, uint64_t v) { return a.off < v; }) - cur->e.begin());
      if (pos < n && cur->e[pos].off == off) { uw_release(S, sl); return false; }
    }
    UWSet* nw = new UWSet; nw->e.reserve(n + 1);
    for (size_t i = 0; i < pos; i++) nw->e.push_back(cur->e[i]);
    nw->e.push_back(UWEnt{off, str});
    for (size_t i = pos; i < n; i++) nw->e.push_back(cur->e[i]);
    if (ss.uw.compare_exchange_strong(cur, nw, std::memory_order_seq_cst)) break;
    delete nw;
  }
  uw_release(S, sl);
  S.uw_n.fetch_add(1, std::memory_order_relaxed); S.uw_bytes.fetch_add((int64_t)len, std::memory_order_relaxed); S.uw_added.fetch_add(1, std::memory_order_relaxed);
  uw_retire(S, ss, cur);
  return true;
}
// the line is on disk: let go of the copy; false = no such key
bool uw_del(Store& S, Sess& ss, uint64_t off) {
  const int sl = uw_slot(S);
  const UWSet* cur; size_t len = 0;
  for (;;) {
    cur = uw_protect(S, ss, sl);
    if (!cur) { uw_release(S, sl); return false; }
    const size_t n = cur->e.size();
    const size_t pos = (size_t)(std::lower_bound(cur->e.begin(), cur->e.end(), off, [](const UWEnt& a, uint64_t v) { return a.off < v; }) - cur->e.begin());
    if (pos >= n || cur->e[pos].off != off) { uw_release(S, sl); return false; }
    len = cur->e[pos].s->size();
    UWSet* nw = nullptr;
    if (n > 1) { nw = new UWSet; nw->e.reserve(n - 1); for (size_t i = 0; i < n; i++) if (i != pos) nw->e.push_back(cur->e[i]); }
    if (ss.uw.compare_exchange_strong(cur, nw, std::memory_order_seq_cst)) break;
    delete nw;
  }
  uw_release(S, sl);
  S.uw_n.fetch_sub(1, std::memory_order_relaxed); S.uw_bytes.fetch_sub((int64_t)len, std::memory_order_relaxed); S.uw_released.fetch_add(1, std::memory_order_relaxed);
  uw_retire(S, ss, cur);
  return true;
}
// the smallest offset of a line that is held (not confirmed written); UINT64_MAX = none
uint64_t uw_min(const Store& S, const Sess& ss) {
  if (!ss.uw.load(std::memory_order_acquire)) return UINT64_MAX;
  const int sl = uw_slot(S);
  const UWSet* cur = uw_protect(S, ss, sl);
  const uint64_t m = cur && !cur->e.empty() ? cur->e.front().off : UINT64_MAX;
  uw_release(S, sl);
  return m;
}
// the copy of the line at off, if it is still held
bool uw_get(const Store& S, const Sess& ss, uint64_t off, std::string& out) {
  if (!ss.uw.load(std::memory_order_acquire)) return false;
  const int sl = uw_slot(S);
  const UWSet* cur = uw_protect(S, ss, sl);
  bool hit = false;
  if (cur) {
    const size_t n = cur->e.size();
    const size_t pos = (size_t)(std::lower_bound(cur->e.begin(), cur->e.end(), off, [](const UWEnt& a, uint64_t v) { return a.off < v; }) - cur->e.begin());
    if (pos < n && cur->e[pos].off == off) { out = *cur->e[pos].s; hit = true; }
  }
  uw_release(S, sl);
  return hit;
}

#ifdef _WIN32  // KW2: thread-local storage through the platform layer
struct vp_tag_tl_lemma {};
#define tl_lemma (vp_tls_get<bool>(vp_tag_tl_lemma{}, false))
#else
thread_local bool tl_lemma = false;  // a lemma-E turn was met by this thread's current call (VI-05: an error value, not an exit)
#endif

// ---------------- turns (D1, D2) and the observer (D3) ----------------
void cut_block(std::string& user, const char* open, const char* close) {  // turnvrsd turn_text cut()
  for (size_t a; (a = user.find(open)) != std::string::npos;) {
    const size_t b = user.find(close, a);
    user.erase(a, b == std::string::npos ? std::string::npos : b + std::strlen(close) - a);
  }
}
void cut_injected(std::string& s) {
  cut_block(s, "<recommended_plugins>", "</recommended_plugins>");
  cut_block(s, "# AGENTS.md instructions", "</INSTRUCTIONS>");
}

// turnvrsd turn_text request: user text without injected blocks (encrypted payload -> first commentary)
std::string req_text(const TurnParse& p) {
  std::string user = p.user;
  cut_injected(user);
  if (const auto q = user.find("Payload:"); user.rfind("Message Type:", 0) == 0 && q != std::string::npos &&
      user.find_first_not_of(" \n\t", q + 8) == std::string::npos && !p.first_assistant.empty())
    user += "(payload encrypted; first commentary)\n" + p.first_assistant;
  return user;
}

// observer v11: S of turn c on the lines of y (a line counts once, at its first cue found in C(c))
// observer v11: S of turn c on the lines of y (a line counts once, at its first cue found in C(c))
uint32_t line_hits(const TurnRec& y, const TurnRec& c) {
  uint32_t s = 0; const uint64_t* d = y.D.data();
  for (uint32_t i = 0; i < y.n; ++i) {
    const uint32_t L = y.len[i];
    for (uint32_t k = 0; k < L; ++k) if (std::binary_search(c.C.begin(), c.C.end(), d[k])) { ++s; break; }
    d += L;
  }
  return s;
}
uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
uint64_t uniform_below(uint64_t& s, uint64_t range) {
  const uint64_t t = (0 - range) % range;
  for (;;) { const uint64_t r = splitmix(s); if (r >= t) return r % range; }
}
double obs_z() { static const double z = standard_normal_quantile(0.5 + OBS_CONF / 2); return z; }

// libvrs: obs_of/direct/ref_turn read the turn table and 𝔅 of the handle (S); lemma E violated -> the rules stay empty (abstain), the
// ObsY is marked bad and the call that meets it fails (VI-05: 42f ended the process here).
// libvrs 5 (D9 / P-9): the observer of y is drawn over the window [k - wn, k) of 𝔅, so it is a function of (the identity of y, the window); phi says which
// window. A cached observer is the observer when its phi is the phi of the window now; otherwise it is made again (built over one window: phi is the same
// before and after). A turn that comes into 𝔅 below the window moves k and the window and changes nothing else.
struct Win { uint32_t k = 0, lo = 0, wn = 0; uint64_t phi = 0; };
inline uint64_t win_phi(uint32_t wn, uint64_t hs) { return ci_mix(hs ^ ((uint64_t(wn) + 1) * 0x9E3779B97F4A7C15ull)); }
Win window_of(const Store& S, const TurnRec& t) {
  uint64_t k, hk; S.B.fp(t.the, k, hk);  // |{r in 𝔅 : θₑ(r) < θₑ(y)}| and the sum of their identity hashes
  Win w; w.k = uint32_t(k); w.lo = w.k > OBS_W ? w.k - OBS_W : 0; w.wn = w.k - w.lo;
  const uint64_t hlo = w.lo ? S.B.hpos(w.lo) : 0;
  w.phi = win_phi(w.wn, hk - hlo);
  return w;
}
// A pass is one piece of reading of 𝔅 (a count, a check of a unit): every window it meets is looked at once and kept; at the end the pass is good when no
// insert into 𝔅 began or was running meanwhile (the two counters of 𝔅), so everything it read is one state of 𝔅. Otherwise the caller does it again.
struct Pass {
  const Store* S = nullptr; uint64_t st0 = 0, fin0 = 0; bool bad = false;
  std::unordered_map<uint32_t, Win> win;
  std::unordered_map<uint32_t, const A0Val*> a0;
  std::vector<std::unique_ptr<A0Val>> a0own;
  bool stable() const { return st0 == fin0 && S->B.started.load(std::memory_order_seq_cst) == st0 && S->B.finished.load(std::memory_order_seq_cst) == fin0; }
};
#ifdef _WIN32  // KW2
struct vp_tag_tl_pass {};
#define tl_pass (vp_tls_get<Pass*>(vp_tag_tl_pass{}, nullptr))
#else
thread_local Pass* tl_pass = nullptr;
#endif
struct PassScope {
  Pass p; Pass* prev;
  explicit PassScope(const Store& S) {
    p.S = &S; p.fin0 = S.B.finished.load(std::memory_order_seq_cst); p.st0 = S.B.started.load(std::memory_order_seq_cst);
    prev = tl_pass; tl_pass = &p;
  }
  ~PassScope() { tl_pass = prev; }
  bool ok() const { return !p.bad && p.stable(); }
};
// N-B: a caller already inside PassScope can carry its own context directly.
Win win_in_pass(const Store& S, uint32_t y, Pass* pass) {
  if (pass) {
    auto it = pass->win.find(y);
    if (it != pass->win.end()) return it->second;
    return pass->win.emplace(y, window_of(S, *S.TT[y])).first->second;
  }
  return window_of(S, *S.TT[y]);
}
Win win_of(const Store& S, uint32_t y) { return win_in_pass(S, y, tl_pass); }
// D3: the rules of y over its window
ObsY* obs_build(const Store& S, uint32_t y) {
  TurnRec& t = *S.TT[y];
  for (;;) {
    const Win w0 = window_of(S, t);
    auto* o = new ObsY; o->n = t.n; o->src = t.src; o->k = w0.k; o->wn = w0.wn; o->phi = w0.phi;
    if (o->k >= OBS_M && o->n >= 4 && o->src >= 2) {
      uint64_t s = OBS_KEY ^ t.seed;
      std::vector<uint32_t> R;
      for (uint32_t j = o->wn - OBS_M; j < o->wn; ++j) {  // Floyd over the window
        const uint32_t r = uint32_t(uniform_below(s, uint64_t(j) + 1));
        R.push_back(std::find(R.begin(), R.end(), r) != R.end() ? j : r);
      }
      o->R = R; std::sort(o->R.begin(), o->R.end());
      std::vector<uint32_t> Sv;
      for (auto r : R) Sv.push_back(line_hits(t, *S.TT[S.B[w0.lo + r]]));
      o->max = *std::max_element(Sv.begin(), Sv.end());
      std::nth_element(Sv.begin(), Sv.begin() + 49, Sv.end());
      o->med = Sv[49];
      const double z = obs_z(), z2 = z * z;
      const auto U = [&](uint32_t m) { return kernel::wilson_interval(double(m), double(o->n - m), z, z2).upper; };
      const double Lmax = kernel::wilson_interval(double(o->max), double(o->n - o->max), z, z2).lower;
      uint32_t lo = o->med, hi = o->med <= o->n - 1 ? o->n - 1 : o->n;
      while (lo < hi) { const uint32_t mid = lo + (hi - lo) / 2; if (Lmax <= U(mid)) hi = mid; else lo = mid + 1; }
      if (!(Lmax <= U(lo))) { o->bad = true; }
      else {
        o->m0 = lo; o->tau = U(lo);
        EvidencePolicy pol; pol.confidence_level = OBS_CONF; pol.axis_count = 1;
        if (o->tau < 1) { pol.chance_rate = o->tau; pol.accept_margin = 0; } else { pol.chance_rate = 0; pol.accept_margin = 1; }
        o->rules = make_evidence_rules(pol);
      }
    }
    const Win w1 = window_of(S, t);
    if (w1.k == w0.k && w1.phi == w0.phi && w1.wn == w0.wn) { if (o->bad) S.lemmaE.fetch_add(1); return o; }  // k too: a turn that came in below the window shifts the positions the draw was read at (the set is the same, the draw is not)
    delete o;  // 𝔅 moved under the draw: again
    S.count_retries.fetch_add(1, std::memory_order_relaxed);
  }
}
const ObsY* obs_of(const Store& S, uint32_t y) {
  TurnRec& t = *S.TT[y];
  const uint64_t phi = win_of(S, y).phi;
  for (;;) {
    ObsY* o = t.obs.load(std::memory_order_acquire);
    if (o && o->phi == phi) return o;
    ObsY* nw = obs_build(S, y);
    if (nw->phi != phi && tl_pass) tl_pass->bad = true;  // 𝔅 moved under the pass: the pass is done again
    ObsY* ex = o;
    if (t.obs.compare_exchange_strong(ex, nw)) {
      if (o) { retire_obj(S, o, 0, sizeof(ObsY) + o->R.size() * 4); S.obs_rebuilt.fetch_add(1, std::memory_order_relaxed); }
      return nw;
    }
    delete nw;  // somebody else made or replaced it first: look again
    if (ex && ex->phi == phi) return ex;
  }
}
// J(x, y): -1 abstain before the core (not in Π), 0 accept, 1 reject, 2 abstain
// D3″: su, sa = the hits on the request-side and answer-side lines of y (su + sa = S_x)
// D3‴: sn = 1 when C(x) meets N(y) (the injections of y that drew no world_state record); J itself does not use it
int direct(const Store& S, uint32_t x, uint32_t y, uint32_t* sx = nullptr, uint32_t* su = nullptr, uint32_t* sa = nullptr, uint32_t* sn = nullptr) {
  const ObsY* o = obs_of(S, y); if (o->bad) tl_lemma = true;
  if (!o->rules) return -1;
  const TurnRec& Y = *S.TT[y]; const TurnRec& Xc = *S.TT[x];
  uint32_t hu = 0, ha = 0; const uint64_t* d = Y.D.data();
  for (uint32_t i = 0; i < Y.n; ++i) {
    const uint32_t L = Y.len[i];
    for (uint32_t k = 0; k < L; ++k) if (std::binary_search(Xc.C.begin(), Xc.C.end(), d[k])) { (i < Y.nu ? hu : ha)++; break; }
    d += L;
  }
  const uint32_t s = hu + ha;
  if (sx) *sx = s;
  if (su) *su = hu;
  if (sa) *sa = ha;
  if (sn) { *sn = 0; for (uint64_t h : Y.Nc) if (std::binary_search(Xc.C.begin(), Xc.C.end(), h)) { *sn = 1; break; } }
  kernel::EvidenceTally t;
  t.axis_support[0] = double(s); t.axis_refute[0] = double(o->n - s);
  t.axis_source_diversity[0] = o->src; t.source_diversity = o->src; t.context_diversity = o->n; t.revision = 1;
  const auto st = kernel::judge_evidence(*o->rules, t).status();
  return st == kernel::EvidenceStatus::accept ? 0 : st == kernel::EvidenceStatus::reject ? 1 : 2;
}
// D5′ / D9 reference turn r(x, y): uniform on E^W_y minus R_y, seeded by the identities of y and x; UINT32_MAX when empty
uint32_t ref_turn(const Store& S, uint32_t x, uint32_t y) {
  const ObsY* o = obs_of(S, y); if (o->bad) tl_lemma = true;
  if (!o->rules || o->wn <= OBS_M) return UINT32_MAX;
  const Win w = win_of(S, y);
  uint64_t xs = S.TT[x]->seed;
  uint64_t s = OBS_REF ^ S.TT[y]->seed ^ splitmix(xs);
  for (;;) {
    const uint32_t p = uint32_t(uniform_below(s, o->wn));
    if (!std::binary_search(o->R.begin(), o->R.end(), p)) {
      const uint32_t r = S.B[w.lo + p];
      if (!r) { if (tl_pass) tl_pass->bad = true; return UINT32_MAX; }
      return r;
    }
  }
}
// libvrs P-8: lin1 is the number of lineage roots with a positive count in the cell (contrib_apply); lin2 is not used
// D5′ cells: alpha = J(x, y) = A, beta = J(r, y) = A
void cell_add(Cell* cells, int idx, bool alpha, bool beta, uint32_t root) {
  cells[idx].an.fetch_add(alpha ? (1ull << 32) + 1 : 1);
  if (alpha != beta) cells[idx].ab.fetch_add(alpha ? (1ull << 32) : 1);
  uint32_t e = 0;
  if (!cells[idx].lin1.compare_exchange_strong(e, root + 1) && e != root + 1) cells[idx].lin2.store(1);
}
constexpr double PAT_CONF = 0.99;
const kernel::EvidenceRules& pattern_rules() {
  static const kernel::EvidenceRules r = [] {
    EvidencePolicy pol; pol.chance_rate = 0.5; pol.accept_margin = 0; pol.confidence_level = PAT_CONF; pol.axis_count = 1;
    return make_evidence_rules(pol);
  }();
  return r;
}
struct PJ { int st; double lower; uint32_t a, n, s, a2, b2; };
PJ cell_judge(const Cell& c) {
  const uint64_t an = c.an.load(), ab = c.ab.load();
  PJ r; r.a = uint32_t(an >> 32); r.n = uint32_t(an & 0xffffffffu);
  r.a2 = uint32_t(ab >> 32); r.b2 = uint32_t(ab & 0xffffffffu);
  r.s = c.lin1.load() == 0 ? 0 : 1 + (c.lin1.load() >= 2 ? 1 : 0);
  kernel::EvidenceTally t;
  t.axis_support[0] = double(r.a2); t.axis_refute[0] = double(r.b2);
  t.axis_source_diversity[0] = r.s; t.source_diversity = r.s; t.context_diversity = std::min<uint32_t>(r.n, 4); t.revision = 1;
  const auto j = kernel::judge_evidence(pattern_rules(), t);
  r.st = j.status() == kernel::EvidenceStatus::accept ? 0 : j.status() == kernel::EvidenceStatus::reject ? 1 : 2;
  static const double z = standard_normal_quantile(0.5 + PAT_CONF / 2), z2 = z * z;
  r.lower = r.n ? kernel::wilson_interval(double(r.a), double(r.n - r.a), z, z2).lower : 0.0;  // strength w = L_z(a, n - a)
  return r;
}

// lineage pieces of a turn view up to index upto (parent pieces keep their fork cut; the own piece stops at upto)
// lineage pieces of a turn view up to index upto (parent pieces keep their fork cut; the own piece stops at upto)
Cuts cuts_of(const View& TV, size_t upto) {
  Cuts c;
  for (size_t k = 0; k < TV.ssg.size() && TV.off[k] < upto; k++) {
    const size_t len = (k + 1 < TV.off.size() ? TV.off[k + 1] : TV.n) - TV.off[k];
    c.push_back({TV.ssg[k], uint32_t(std::min(len, upto - TV.off[k]))});
  }
  return c;
}
// D4″: sh(x, y) for every lineage turn y that shares a rare cue (df_t <= F) with Q (L2 v3: F + 1 steps per piece at most counted)
std::unordered_map<uint32_t, uint32_t> content_sh(const Store& S, const std::vector<uint64_t>& Q, const Cuts& cuts, uint64_t t) {
  std::unordered_map<uint32_t, uint32_t> sh;
  std::vector<uint32_t> ys;
  for (uint64_t h : Q) {
    ys.clear(); bool common = false;
    for (auto& [sg, cut] : cuts) {
      const PSlot* s = S.pt_find(pt_key(h, sg));
      if (!s) continue;
      for (const PNode* n = s->head.load(std::memory_order_acquire); n && n->tpos < cut; n = n->next.load(std::memory_order_acquire)) {
        const uint32_t y = S.tl[sg][n->tpos];
        if (S.TT[y]->the > t) continue;
        if (ys.size() == RARE_F) { common = true; break; }
        ys.push_back(y);
      }
      if (common) break;
    }
    if (!common) for (uint32_t y : ys) sh[y]++;
  }
  return sh;
}
// L1 v27 D5¹¹: the rare cues h of Q with their turns Y(h) (df_t(h) = |Y(h)| <= F), the same walk as content_sh
std::vector<std::pair<uint64_t, std::vector<uint32_t>>> rare_lists(const Store& S, const std::vector<uint64_t>& Q, const Cuts& cuts, uint64_t t) {
  std::vector<std::pair<uint64_t, std::vector<uint32_t>>> out;
  for (uint64_t h : Q) {
    std::vector<uint32_t> ys; bool common = false;
    for (auto& [sg, cut] : cuts) {
      const PSlot* s = S.pt_find(pt_key(h, sg));
      if (!s) continue;
      for (const PNode* n = s->head.load(std::memory_order_acquire); n && n->tpos < cut; n = n->next.load(std::memory_order_acquire)) {
        const uint32_t y = S.tl[sg][n->tpos];
        if (S.TT[y]->the > t) continue;
        if (ys.size() == RARE_F) { common = true; break; }
        ys.push_back(y);
      }
      if (common) break;
    }
    if (!common && !ys.empty()) out.push_back({h, std::move(ys)});
  }
  return out;
}
// L1 v5 key layout: distance 0..63, origin 64..127, content 128..143, neighbour use 144..151
// D4/D4″ base pairs of turn x at lineage index ix: every y of BC(x) with its position and content keys (no neighbour-use keys)
struct BP { uint32_t y; size_t j; int nk; int keys[3]; uint32_t c; };
std::vector<BP> base_pairs(const Store& S, uint32_t x, const View& TV, size_t ix) {
  const TurnRec& X = *S.TT[x];
  const auto sh = content_sh(S, X.Q, cuts_of(TV, ix), X.th0);
  std::vector<std::pair<uint32_t, size_t>> cand;  // (y, lineage index)
  std::unordered_set<uint32_t> pos;
  size_t lo = ix > (size_t)KD ? ix - KD : 0;
  for (size_t j = 0; j < ix; j++) {
    if (j >= (size_t)KO && j < lo) { j = lo - 1; continue; }
    cand.push_back({TV[j], j}); pos.insert(TV[j]);
  }
  for (auto& [y, c] : sh) if (!pos.count(y)) cand.push_back({y, TV.idx(S.TT[y]->sg, S.TT[y]->tpos)});
  std::vector<BP> out;
  for (auto& [y, j] : cand) {
    BP b{y, j, 0, {0, 0, 0}, 0};
    if (S.TT[y]->the < X.the) {
      if (ix - j <= (size_t)KD) b.keys[b.nk++] = int(ix - j - 1);
      if (j < (size_t)KO && (j >= ix || ix - j < (size_t)FAR_MAX)) b.keys[b.nk++] = KD + int(j);  // D4′: lineage-distance horizon 4096
    }
    const auto it = sh.find(y);
    b.c = it == sh.end() ? 0 : it->second;
    if (b.c && S.TT[y]->the <= X.th0 && (j >= ix || ix - j < (size_t)FAR_MAX)) b.keys[b.nk++] = KD + KO + int(std::min<uint32_t>(b.c, KC)) - 1;
    if (b.nk) out.push_back(b);
  }
  return out;
}
// libvrs 5: A0(x) is made from the observers of its candidates BC(x), so it is as good as the window fingerprints of those: it keeps the turns of BC(x) and the sum
// of their fingerprints, and is made again when the sum of the fingerprints now is another one. `vstamp` says "all of 𝔅 was the same when this was last
// found good": while no turn comes into 𝔅 the check is one comparison. (the old one is kept until its epoch ends, a reader may hold it)
const A0Val* a0_of(const Store& S, uint32_t x, const View& TV, size_t ix) {
  TurnRec& X = *S.TT[x];
  if (tl_pass) { auto it = tl_pass->a0.find(x); if (it != tl_pass->a0.end()) return it->second; }
  for (;;) {
    A0Val* p = X.a0.load(std::memory_order_acquire);
    if (p) {
      const uint64_t f0 = S.B.finished.load(std::memory_order_seq_cst), s0 = S.B.started.load(std::memory_order_seq_cst);
      if (f0 == s0 && p->vstamp.load(std::memory_order_acquire) == f0 + 1) { if (tl_pass) tl_pass->a0[x] = p; return p; }
      uint64_t ph = 0; for (uint32_t y : p->Yb) ph += win_of(S, y).phi;
      if (ph == p->phi) {
        if (f0 == s0 && S.B.started.load(std::memory_order_seq_cst) == s0 && S.B.finished.load(std::memory_order_seq_cst) == f0 && (!tl_pass || (tl_pass->fin0 == f0 && tl_pass->st0 == s0)))
          p->vstamp.store(f0 + 1, std::memory_order_release);  // (inside a pass ph is the sum at the pass start: stamping it for a later state would call an old A0 good there)
        if (tl_pass) tl_pass->a0[x] = p;
        return p;
      }
    }
    auto* nv = new A0Val;
    const auto bp = base_pairs(S, x, TV, ix);
    for (auto& b : bp) nv->Yb.push_back(b.y);
    std::sort(nv->Yb.begin(), nv->Yb.end()); nv->Yb.erase(std::unique(nv->Yb.begin(), nv->Yb.end()), nv->Yb.end());
    for (uint32_t y : nv->Yb) nv->phi += win_of(S, y).phi;
    for (auto& b : bp) if (direct(S, x, b.y) == 0) nv->v.push_back(b.y);
    std::sort(nv->v.begin(), nv->v.end());
    if (tl_pass && !tl_pass->stable()) {  // 𝔅 moved while it was made: it is not kept; the pass is done again
      tl_pass->bad = true; tl_pass->a0own.emplace_back(nv); tl_pass->a0[x] = nv; return nv;
    }
    A0Val* ex = p;
    if (X.a0.compare_exchange_strong(ex, nv)) {
      if (p) { retire_obj(S, p, 1, sizeof(A0Val) + (p->v.size() + p->Yb.size()) * 4); S.a0_rebuilt.fetch_add(1, std::memory_order_relaxed); }
      if (tl_pass) tl_pass->a0[x] = nv;
      return nv;
    }
    delete nv;
  }
}
// all keys of the candidates of the turn at lineage index ix: BC keys plus neighbour-use keys v(d) for x₋d with θₑ <= tq
struct CK { size_t j; int nk = 0; int keys[3 + KV + 1]; uint32_t c = 0; };
// plus far distance keys (L1 v7) for 64 < ix - j < 4096 with θₑ(y) < tf (strict, closed-pair counting) or <= tf (request)
// libvrs 5: `ystat` (if given) collects Y(x), the turns whose observers the result can depend on: the candidates, the far ones, and every turn of BC(x₋d) (A0 is
// a subset of it); it does not depend on any observer, only on the turns of the lineage.
std::unordered_map<uint32_t, CK> all_keys(const Store& S, const View& TV, size_t ix, std::vector<BP> base, uint64_t tq,
                                          uint64_t tf, bool strict, std::vector<uint32_t>* ystat = nullptr) {
  std::unordered_map<uint32_t, CK> m;
  for (auto& b : base) { CK k; k.j = b.j; k.c = b.c; for (int q = 0; q < b.nk; q++) k.keys[k.nk++] = b.keys[q]; m[b.y] = k; if (ystat) ystat->push_back(b.y); }
  for (size_t j = ix > FAR_MAX - 1 ? ix - (FAR_MAX - 1) : 0; j + KD < ix; j++) {
    const uint32_t y = TV[j];
    if (strict ? !(S.TT[y]->the < tf) : !(S.TT[y]->the <= tf)) continue;
    if (ystat) ystat->push_back(y);
    const int b = 63 - __builtin_clzll((unsigned long long)(ix - j));
    auto it = m.find(y);
    if (it == m.end()) { CK k; k.j = j; it = m.emplace(y, k).first; }
    it->second.keys[it->second.nk++] = KD + KO + KC + KV + b - 6;
  }
  for (int d = 1; d <= KV && (size_t)d <= ix; d++) {
    const uint32_t xp = TV[ix - d];
    if (S.TT[xp]->the > tq) continue;
    const A0Val* a0 = a0_of(S, xp, TV, ix - d);
    if (ystat) ystat->insert(ystat->end(), a0->Yb.begin(), a0->Yb.end());
    for (uint32_t y : a0->v) {
      auto it = m.find(y);
      if (it == m.end()) { CK k; k.j = TV.idx(S.TT[y]->sg, S.TT[y]->tpos); it = m.emplace(y, k).first; }
      it->second.keys[it->second.nk++] = KD + KO + KC + d - 1;
    }
  }
  return m;
}
// D6‴: needed turns of the open turn at lineage index i0, by density (L1 v4) then lineage index.
// Position keys need θₑ <= t; content keys use Q at tq; neighbour-use keys need θₑ(x₋d) <= tq.
struct Need { double w; size_t j; uint32_t y; double rho = 0; int part = 0; double phat = 0; uint32_t cn = 0; uint64_t tok = 0; bool bycontent = false; };  // bycontent: L1 v26 D5¹⁰ (ii)  // phat, cn: measurement only
// D2⁗ VI^part(y): part 0 = the utterance items, 1 = the final assistant item, 2 = the injections that drew no world_state record
bool in_part(uint8_t kind, int part) { return part == 0 ? kind == K_USER : part == 1 ? kind == K_ASSIST : kind == K_DEV; }
std::vector<uint32_t> vi_part(const Store& S, uint32_t y, int part) {
  std::vector<uint32_t> o;
  for (uint32_t z : S.TT[y]->vi) if (in_part(S.it[z].kind, part)) o.push_back(z);
  return o;
}
// D5⁗ size class σ = min(7, floor(log4 max(1, tok))) of an element (y, part)
int size_class(uint64_t tok) { int b = 0; while (b < NS - 1 && tok >= (1ull << (2 * (b + 1)))) b++; return b; }
uint64_t part_tok(const Store& S, uint32_t y, int part, bool* any = nullptr) {
  uint64_t c = 0; bool a = false;
  for (uint32_t z : S.TT[y]->vi) if (in_part(S.it[z].kind, part)) { c += S.it[z].tok; a = true; }
  if (any) *any = a;
  return c;
}
// libvrs: need_list lost its measurement-only `keyed` output.
std::vector<Need> need_list(const Store& S, const View& TV, size_t i0, uint64_t t, const Cell* cells,
                            const std::vector<uint64_t>& Q, uint64_t tq) {
  std::vector<BP> base;
  {  // BC of the open turn: position candidates (θₑ <= t) and content candidates (Q at tq)
    std::unordered_map<uint32_t, uint32_t> sh;
    if (!Q.empty()) sh = content_sh(S, Q, cuts_of(TV, i0), tq);
    std::unordered_set<uint32_t> pos;
    size_t lo = i0 > (size_t)KD ? i0 - KD : 0;
    for (size_t j = 0; j < i0; j++) {
      if (j >= (size_t)KO && j < lo) { j = lo - 1; continue; }
      const uint32_t y = TV[j]; pos.insert(y);
      BP b{y, j, 0, {0, 0, 0}, 0};
      if (S.TT[y]->the <= t) {
        if (i0 - j <= (size_t)KD) b.keys[b.nk++] = int(i0 - j - 1);
        if (j < (size_t)KO) b.keys[b.nk++] = KD + int(j);
      }
      const auto it = sh.find(y);
      if (it != sh.end() && S.TT[y]->the <= tq) { b.c = it->second; b.keys[b.nk++] = KD + KO + int(std::min<uint32_t>(b.c, KC)) - 1; }
      if (b.nk) base.push_back(b);
    }
    for (auto& [y, c] : sh) if (!pos.count(y) && S.TT[y]->the <= tq)
      base.push_back({y, TV.idx(S.TT[y]->sg, S.TT[y]->tpos), 1, {KD + KO + int(std::min<uint32_t>(c, KC)) - 1, 0, 0}, c});
  }
  std::vector<Need> out;
#ifdef _WIN32  // KW2
  std::vector<double>& cw = VP_TLS(std::vector<double>, NCA);
  std::vector<int>& cst = VP_TLS(std::vector<int>, NCA);
  std::vector<double>& cph = VP_TLS(std::vector<double>, NCA);
  std::vector<uint32_t>& cnn = VP_TLS(std::vector<uint32_t>, NCA);
#else
  static thread_local std::vector<double> cw(NCA);  // each cell judged once per request (E2-20)
  static thread_local std::vector<int> cst(NCA);
  static thread_local std::vector<double> cph(NCA);
  static thread_local std::vector<uint32_t> cnn(NCA);
#endif
  for (int k = 0; k < NCA; k++) { PJ p = cell_judge(cells[k]); cst[k] = p.st; cw[k] = p.st == 0 ? p.lower : -1; cph[k] = p.n ? double(p.a) / p.n : 0; cnn[k] = p.n; }  // L1 v25 D5⁹ (i): rule'
  double pool_p = 0, pool_t = 0;  // D5⁹ (ii): the candidates' use per token, rho-bar = sum p-hat / sum t
  for (auto& [y, k] : all_keys(S, TV, i0, std::move(base), tq, t, false))
    for (int part = 0; part < NP; part++) {
      bool any = false;
      const uint64_t c = part_tok(S, y, part, &any);
      if (!any) continue;  // an empty part is not an element
      const int off = part * NCELL + size_class(c) * NC2;
      double w = -1; int wc = -1;
      for (int q = 0; q < k.nk; q++) {  // D5⁵: the finest decided cell; abstain (U) falls back to the coarse cell
        const int f = NC2 + k.keys[q] + off, g = k.keys[q] + part * NCELL;
        const int use = cst[f] == 0 ? f : cst[f] == 1 ? -1 : g;
        if (use >= 0 && cw[use] > w) { w = cw[use]; wc = use; }
      }
      {  // p-hat of the element for the pool: its finest decided cell (coarse when the fine one abstains), largest over keys
        double ph = 0;
        for (int q = 0; q < k.nk; q++) { const int f = NC2 + k.keys[q] + off, g = k.keys[q] + part * NCELL; ph = std::max(ph, cph[cst[f] == 2 ? g : f]); }
        if (wc >= 0) ph = std::max(ph, cph[wc]);
        pool_p += ph; pool_t += double(std::max<uint64_t>(c, 1));
      }
      if (w < 0) continue;
      bool bc = false;  // D5¹⁰ (ii): approved through a content key (the present's rare-cue share with y)
      for (int q = 0; q < k.nk; q++) { const int key = k.keys[q]; if (key < KD + KO || key >= KD + KO + KC) continue;
        const int f = NC2 + key + off, g = key + part * NCELL; const int use = cst[f] == 0 ? f : cst[f] == 1 ? -1 : g; if (use >= 0 && cw[use] >= 0) bc = true; }
      out.push_back({w, k.j, y, cph[wc] / double(std::max<uint64_t>(c, 1)), part, cph[wc], cnn[wc], c, bc});  // L2 v19 E2-49: rank by π̂/tok (P17), approval unchanged
    }
  (void)pool_p; (void)pool_t;  // L1 v26: D5⁹ (ii) withdrawn (a relative bar, §95)
  // L1 v4 D7: density rho = w / c descending, then lineage index descending
  std::sort(out.begin(), out.end(), [](const Need& a, const Need& b) { return a.rho != b.rho ? a.rho > b.rho : a.j > b.j; });
  return out;
}
// D5′ counting for one closed turn x at lineage index ix: every candidate y once, into each of its keys' cells.
// libvrs: no dump; `rec` collects (cell, flags, root) of every cell_add (the contribution record of P-1).
void count_pairs(const Store& S, uint32_t x, const View& TV, size_t ix, uint32_t root, Cell* cells, Cell* cells2 = nullptr,
                 std::vector<std::array<uint32_t, 3>>* rec = nullptr, std::vector<uint32_t>* ystat = nullptr) {
  for (auto& [y, k] : all_keys(S, TV, ix, base_pairs(S, x, TV, ix), S.TT[x]->th0, S.TT[x]->the, true, ystat)) {
    const uint32_t r = ref_turn(S, x, y);
    if (r == UINT32_MAX) continue;  // y abstains before the core, or E^W_y minus R_y is empty
    uint32_t sx = 0, sr = 0, xu = 0, xa = 0, xn = 0, ru = 0, ra = 0, rn = 0;
    const int v = direct(S,x, y, &sx, &xu, &xa, &xn), vr = direct(S,r, y, &sr, &ru, &ra, &rn);
    bool hu = false, ha = false, hn = false;  // D5⁶: cell (key, part, σ(y, part)); empty parts are not elements
    const int su = size_class(part_tok(S, y, 0, &hu)), sa = size_class(part_tok(S, y, 1, &ha)), sn = size_class(part_tok(S, y, 2, &hn));
    const bool has[NP] = {hu, ha, hn}; const int sc[NP] = {su, sa, sn};
    const bool al[NP] = {v == 0 && xu > 0, v == 0 && xa > 0, v == 0 && xn > 0}, be[NP] = {vr == 0 && ru > 0, vr == 0 && ra > 0, vr == 0 && rn > 0};
    for (int q = 0; q < k.nk; q++)
      for (int part = 0; part < NP; part++) {  // D5″/D3‴: alpha = U^part(x, y), beta = U^part(r, y)
        if (!has[part]) continue;
        const int ci[2] = {k.keys[q] + part * NCELL, NC2 + k.keys[q] + part * NCELL + sc[part] * NC2};
        for (int c : ci) {
          for (Cell* cs : {cells, cells2}) if (cs) cell_add(cs, c, al[part], be[part], root);
          if (rec) rec->push_back({uint32_t(c), uint32_t(al[part]) | uint32_t(be[part]) << 1, root});
        }
      }
  }
}

// ---------- assembly (L1 section 1, L2 section 3); read-only over the store ----------
struct Asm {
  std::vector<uint32_t> ids;  // chosen, key order
  uint64_t tokens = 0;
  bool c3 = false, c3_undecidable = false, c3_na = false;  // C3': every user utterance with theta <= t is in  // c3_na: no user message with theta <= t yet
  std::vector<std::pair<uint32_t, int>> mem;  // synapse: the chosen memory elements (y, part), density order (L2 v9 E2-12)
  bool overflow = false;                      // L2 v13 E2-22: the required present input does not fit in B
  std::vector<Need> cand;                     // measurement only (eval dump): the whole need list in its order
  std::vector<std::array<int64_t, 4>> keyed;  // measurement only (r3 §6.3): every keyed element and its decision
  uint64_t mem_tok = 0, present_tok = 0;      // m_s, and the present-input share
  uint64_t x0_tok = 0, i_tok = 0; uint32_t x0_n = 0, i_n = 0;  // measurement only: the two required parts (x0 = h, σ*, U_x)
  uint64_t trace_tok = 0; uint32_t trace_in = 0; bool trace_cut = false;  // measurement only: L2 v17 E2-40
  uint32_t ocalls = 0;                                                     // measurement only: D9′ steps with an unfinished call
  uint32_t rctx = UINT32_MAX; uint64_t rctx_tok = 0;                       // L1 v24 D5⁸ (R): the reply context A(y) and its tokens
  uint32_t closure_add = 0;                                                // measurement only: E2-62 items added by the closure
  uint32_t far_n = 0, n3_ans = 0; uint64_t far_tok = 0, n3_tok = 0;        // L1 v26 D5¹⁰: the far set and the near set (3판 E3)
  uint32_t seg_rep = 0; bool segwin = false;                             // 4판: reports before the window, window on
  uint32_t far_rc = 0, far_uc = 0;                                         // measurement only (L1 v27): rare cues of Q, uncovered ones
};

// read the selected originals from the rollout files (warm); one positioned read per item
std::vector<std::string> fetch(const Store& S, const std::vector<uint32_t>& ids) {
  std::vector<std::string> out; out.reserve(ids.size());
  for (uint32_t z : ids) {
    const Item& m = S.it[z];
    if (S.sx(m.sess).memmode) { out.push_back(m.mem); continue; }  // libvrs: an ephemeral thread keeps its lines in memory (P-4)
    { std::string mem; if (uw_get(S, S.sx(m.sess), m.off, mem)) { out.push_back(std::move(mem)); continue; } }  // P-3': not confirmed written yet: the copy from vrs_insert
    const auto source = source_fd(S.sx(m.sess));
    const int fd = source ? source->fd : -1;
    std::string buf(m.len, '\0');
    ssize_t got = fd >= 0 ? vp_pread(fd, &buf[0], m.len, m.off) : 0;
    buf.resize(got > 0 ? (size_t)got : 0);
    out.push_back(std::move(buf));
  }
  return out;
}

// L1 v28 D6¹³ / L2 v28 E2-66..68: a delivered partial render sends only the lines of its standing units (and its non-unit lines);
// a text with no standing unit is sent empty, a message whose texts are all empty is not sent. Standing is L10″ with the maxima
// taken over the delivered instruction messages (P32). Lines are cut from the JSON-encoded text as they are (E2-67).
uint64_t unit_key(std::string_view ln, uint8_t unit, bool* is_unit) {  // ln decoded; the same keys as part_of's items()/lines()
  *is_unit = false;
  if (unit == 4) {
    size_t a = 0, z = ln.size();
    while (a < z && (ln[a] == ' ' || ln[a] == '\t' || ln[a] == '\r')) a++;
    while (z > a && (ln[z - 1] == ' ' || ln[z - 1] == '\t' || ln[z - 1] == '\r')) z--;
    if (z <= a || ln.substr(a, z - a).find("tokens truncated\xE2\x80\xA6") != std::string_view::npos) return 0;
    *is_unit = true; return fnv(ln.data() + a, z - a);
  }
  const bool ns = unit >= 2, skip_cut = unit == 3;
  const size_t e = ln.size();
  if (!(e > 2 && ln[0] == '-' && ln[1] == ' ')) return 0;
  if (skip_cut && e >= 3 && ln.compare(e - 3, 3, "...") == 0) return 0;
  size_t ke = e;
  if (ns) { const size_t c = ln.find(": ", 2); if (c != std::string_view::npos) ke = c; }
  *is_unit = true; return fnv(ln.data() + 2, ke - 2);
}
void residue(const Store& S, const std::vector<uint32_t>& ids, std::vector<std::string>& lines, uint64_t* cut_bytes, uint32_t* changed, uint32_t* dropped) {
  std::unordered_map<uint64_t, InstrState::Subj> c;
  for (uint32_t m : ids) if (S.it[m].kind == K_DEV) for (const auto& p : S.it[m].parts) if (p.subj != 0 && p.mode == MD_FULL) {
    auto& q = c[p.subj]; if (!q.has || S.it[m].th > q.cmax) { q.has = true; q.cmax = S.it[m].th; q.rho = S.it[m].rs; } }
  for (uint32_t m : ids) if (S.it[m].kind == K_DEV) for (const auto& p : S.it[m].parts) if (p.subj != 0 && p.mode == MD_INC) {
    auto& q = c[p.subj];
    for (uint64_t k : p.keys) { auto ins = q.key.emplace(k, InstrState::KS{S.it[m].th, S.it[m].rs}); if (!ins.second && S.it[m].th > ins.first->second.th) ins.first->second = InstrState::KS{S.it[m].th, S.it[m].rs}; }
  }
  auto stands = [&](uint64_t th, uint64_t k, const InstrState::Subj& q) {  // L10″ for one key (InstrState::part_alive)
    const auto e = q.key.find(k);
    const bool kh = e != q.key.end() && (!q.has || e->second.th > q.cmax);
    return kh ? th >= e->second.rho : (!q.has || th >= q.rho);
  };
  for (size_t q = 0; q < ids.size(); q++) {
    const Item& m = S.it[ids[q]];
    if (m.kind != K_DEV) continue;
    bool any = false; for (const auto& p : m.parts) if (p.subj != 0 && (p.mode == MD_FULL || p.mode == MD_INC)) any = true;
    if (!any) continue;
    const std::string& L = lines[q];
    const size_t ca = L.find("\"content\":[");
    if (ca == std::string::npos) continue;
    std::string out; size_t from = 0; bool ch = false, all_empty = true;
    size_t i = ca + 11, n = 0;
    while (i < L.size() && L[i] != ']') {
      while (i < L.size() && (L[i] == ',' || L[i] == ' ')) i++;
      if (i >= L.size() || L[i] != '{') break;
      const size_t e = json_skip(L, i);
      const size_t tx = L.find("\"text\":\"", i);
      if (tx == std::string::npos || tx >= e) { n++; i = e; all_empty = false; continue; }
      const size_t vb = tx + 8, ve = json_skip(L, tx + 7) - 1;  // the encoded string body [vb, ve)
      const Item::Part* P = n < m.parts.size() ? &m.parts[n] : nullptr;
      if (!P || P->subj == 0 || (P->mode != MD_FULL && P->mode != MD_INC)) { if (ve > vb) all_empty = false; n++; i = e; continue; }
      const auto cs = c.find(P->subj);
      bool part_stands = cs == c.end();  // D6¹³′: a part that does not stand (L10″) is not sent, whatever its form
      if (!part_stands) {
        if (P->mode == MD_FULL) part_stands = !cs->second.has || m.th >= cs->second.rho;
        else for (uint64_t k : P->keys) if (stands(m.th, k, cs->second)) { part_stands = true; break; }
      }
      if (part_stands && !P->unit) { if (ve > vb) all_empty = false; n++; i = e; continue; }  // a standing part without units goes whole
      std::string body; bool stood = false, started = false;
      if (part_stands) for (size_t a = vb;;) {  // E2-67: a line ends at an escaped \n; the segment after the last one is a line too
        size_t b = a;
        while (b < ve && !(L[b] == '\\' && b + 1 < ve && L[b + 1] == 'n')) b += L[b] == '\\' ? 2 : 1;
        if (b > ve) b = ve;
        const std::string_view raw(L.data() + a, b - a);
        bool iu = false;
        const uint64_t k = unit_key(json_string(raw, 0), P->unit, &iu);
        const bool keep = !iu || cs == c.end() || stands(m.th, k, cs->second);
        if (iu && keep) stood = true;
        if (keep) { if (started) body += "\\n"; body.append(raw); started = true; }
        if (b >= ve) break;
        a = b + 2;
      }
      if (!stood) body.clear();
      if (body.size() != ve - vb) { out.append(L, from, vb - from); out += body; from = ve; ch = true; *cut_bytes += (ve - vb) - body.size(); }
      if (!body.empty()) all_empty = false;
      n++; i = e;
    }
    if (all_empty) { lines[q].clear(); (*dropped)++; continue; }
    if (ch) { out.append(L, from, std::string::npos); lines[q] = std::move(out); (*changed)++; }
  }
}

uint64_t call_key(uint32_t sg, uint64_t cid) { return ci_mix(cid ^ (uint64_t(sg) * 0x9E3779B97F4A7C15ull)); }

struct Ctx {
  const Store& S;
  const Sess& own;     // the requesting session
  const Snap& sn;      // its published snapshot (what 42f read from `own->tp`, `own->ist`, the list sizes)
  const std::vector<std::array<uint32_t, 4>>* segs;  // inherited prefix (empty before segok)
  const Cell* cells;
  Ctx(const Store& s, const Sess& o, const Snap& n, const std::vector<std::array<uint32_t, 4>>* sg) : S(s), own(o), sn(n), segs(sg), cells(s.cells) {}
  // the block of item m: m and its call/output partners with theta <= t
  // libvrs: the partners are the chain of (session, call_id); 42f's base/tail call tables gave the same set per session
  void block(uint32_t m, uint64_t t, std::vector<uint32_t>& out) const {
    out.clear(); out.push_back(m);
    const Item& im = S.it[m];
    if (!im.cid) return;
    const uint32_t ms = im.sess;  // L3 v29: partners of the same flow only (D8)
    const auto* nd = S.calls.find(call_key(ms, im.cid));
    if (!nd) return;
    for (uint32_t z = nd->val.head.load(std::memory_order_acquire); z != NONE32; z = S.it[z].nxt.load(std::memory_order_acquire))
      if (z != m && S.it[z].th <= t && S.it[z].sess == ms) out.push_back(z);
    // libvrs 5: the group is a stack (the newest item first); the partners are given in the order of their lines, as the 4th stage appended them
    std::sort(out.begin() + 1, out.end(), [&](uint32_t a, uint32_t b) { return S.it[a].off < S.it[b].off; });
  }
  // longest prefix (by blocks) of list under cap, items in `taken` are not counted again
  uint64_t take_prefix(const std::vector<uint32_t>& list, uint64_t t, uint64_t cap, std::set<uint32_t>& taken) const {
    uint64_t used = 0; std::vector<uint32_t> bl;
    for (uint32_t m : list) {
      if (taken.count(m)) continue;
      block(m, t, bl);
      uint64_t sz = 0; for (uint32_t z : bl) if (!taken.count(z)) sz += S.it[z].tok;
      if (used + sz > cap) break;
      used += sz; for (uint32_t z : bl) taken.insert(z);
    }
    return used;
  }
  // VRS_시냅스조립_v1.md D6/D7 (the synapse window; 42f's other windows are not libvrs's business)
  Asm assemble(uint32_t sg, uint64_t t, uint64_t B) const {
    Asm A;
    const auto& TT = S.TT;
    View L, U;
    const std::vector<std::array<uint32_t, 4>>* lsg = segs;
    for (auto& g : *lsg) { L.add(g[0], &S.sess[g[0]], g[1]); U.add(g[0], &S.usr[g[0]], g[2]); }
    L.add(sg, &own.sess, sn.nsess); U.add(sg, &own.usr, sn.nusr);
    // 7th stage D15/D16 (VRS_코덱스일체화_v1.md §9): the view time thv(x) = max(theta*(x), M_k) (M_k: the latest theta* of the segments before x's),
    // the present t = thv(request line); bounds inside the view by view position (D17)
    std::vector<uint32_t> vs_sg, vs_n; std::vector<size_t> vs_off; std::vector<uint64_t> Mb;
    {
      uint64_t mx = 0; size_t o = 0;
      if (lsg) for (auto& g : *lsg) {
        vs_sg.push_back(g[0]); vs_n.push_back(g[1]); vs_off.push_back(o); Mb.push_back(mx); o += g[1];
        if (g[1]) mx = std::max<uint64_t>(mx, S.it[S.sess[g[0]][g[1] - 1]].th);
        if (g[3]) mx = std::max<uint64_t>(mx, S.TT[S.tl[g[0]][g[3] - 1]]->the);
      }
      vs_sg.push_back(sg); vs_n.push_back(uint32_t(sn.nsess)); vs_off.push_back(o); Mb.push_back(mx);
    }
    t = std::max<uint64_t>(t, Mb.back());
    auto vseg = [&](uint32_t m) -> int { const uint32_t s0 = S.it[m].sess, p0 = S.it[m].pos;
      for (size_t k = 0; k < vs_sg.size(); k++) if (vs_sg[k] == s0) return p0 < vs_n[k] ? int(k) : -1;
      return -1; };
    auto thv = [&](uint32_t m) -> uint64_t { const uint64_t th = S.it[m].th; const int k = vseg(m); return k < 0 ? th : std::max<uint64_t>(th, Mb[k]); };
    auto vpos = [&](uint32_t m) -> size_t { const int k = vseg(m); return k < 0 ? SIZE_MAX : vs_off[k] + S.it[m].pos; };
    // D17 order; VRS_onePC_v1.md B5: an item outside the view is placed by (its file's store name, its place in that file), not by its id
    auto oless = [&](uint32_t a, uint32_t b) {
      const uint64_t ta = thv(a), tb = thv(b);
      if (ta != tb) return ta < tb;
      const size_t va = vpos(a), vb = vpos(b);
      if (va != SIZE_MAX || vb != SIZE_MAX) return va != vb && va < vb;  // the view first, in view order
      const Item& ia = S.it[a]; const Item& ib = S.it[b];
      if (ia.sess != ib.sess) return vp_path(S.sx(ia.sess).nu).compare(vp_path(S.sx(ib.sess).nu)) < 0;
      return ia.pos < ib.pos;
    };
    // last position with theta <= t (binary search; D-ctx-01); 7th stage: by thv, which does not go down along the view (lemma 7-1)
    auto ub = [&](const View& v) { size_t lo = 0, h = v.size(); while (lo < h) { size_t md = (lo + h) / 2; if (thv(v[md]) <= t) lo = md + 1; else h = md; } return lo; };
    size_t hi = ub(L);
    size_t uh = ub(U);
    std::set<uint32_t> taken;
    uint64_t used = 0;
    {
      View TV;
      for (auto& g : *lsg) TV.add(g[0], &S.tl[g[0]], g[3]);
      TV.add(sg, &own.tl, sn.ntl);
      size_t i0 = TV.size();
      const bool open = sn.open;
      const size_t from = open ? L.idx(sg, sn.pos0) : (i0 ? L.idx(TT[TV[i0 - 1]]->sg, TT[TV[i0 - 1]]->pos1) : 0);
      std::vector<uint32_t> P;
      // L2 v17 E2-38/39 (D8, D9): Req = the head h, the latest response's step σ*, the turn's utterances U_x, then I
      auto model = [&](uint32_t m) { const uint8_t k = S.it[m].kind; return k == K_REASON || k == K_ASSIST || k == K_FC || k == K_CTC; };
      size_t f = from; while (f < hi && !model(L[f])) f++;  // head end: the first model item (cost |h|)
      size_t rs0 = hi;                                       // start of the latest response r* (cost |σ*|)
      if (f < hi) { size_t z = hi; while (z > f && !model(L[z - 1])) z--; rs0 = z; while (rs0 > f && model(L[rs0 - 1])) rs0--; }
      // L2 v22 E2-56 (D8): step(r) = the response's model run, the outputs of its calls (by call_id, wherever they are), and the
      // non-model items before the next response except outputs of calls of another response of this turn (P14′)
      std::vector<uint32_t> sbl;
      auto step_of = [&](size_t b, std::vector<uint32_t>& out) {
        out.clear();
        size_t rb = b; while (rb < hi && model(L[rb])) rb++;
        size_t nb = rb; while (nb < hi && !model(L[nb])) nb++;
        for (size_t p = b; p < rb; p++) { block(L[p], t, sbl); out.insert(out.end(), sbl.begin(), sbl.end()); }
        for (size_t p = rb; p < nb; p++) {
          const uint32_t z = L[p]; const uint8_t kd = S.it[z].kind;
          block(z, t, sbl);
          if (kd == K_FCO || kd == K_CTCO) {
            bool other = false;
            for (uint32_t c : sbl) {
              if (S.it[c].kind != K_FC && S.it[c].kind != K_CTC) continue;
              const size_t q = L.idx(S.it[c].sess, S.it[c].pos);
              if (q < L.size() && L[q] == c && q >= f && q < hi && (q < b || q >= rb)) other = true;
            }
            if (other) continue;  // it belongs to its call's step
          }
          out.insert(out.end(), sbl.begin(), sbl.end());
        }
        std::sort(out.begin(), out.end(), [&](uint32_t a, uint32_t c) { return S.it[a].th > S.it[c].th; });
        out.erase(std::unique(out.begin(), out.end()), out.end());
      };
      std::vector<uint32_t> stp;
      if (rs0 < hi) { step_of(rs0, stp); P.insert(P.end(), stp.begin(), stp.end()); }  // σ*, newest first
      if (from < hi) {  // U_x: the session's utterances with θ in [θ(first item of x), t] (binary search, cost log + |U_x|)
        size_t lo = 0, h2 = uh; while (lo < h2) { size_t md = (lo + h2) / 2; if (vpos(U[md]) < from) lo = md + 1; else h2 = md; }  // D17: by view position
        for (size_t q = uh; q > lo; q--) P.push_back(U[q - 1]);
      }
      for (size_t p = f; p > from; p--) P.push_back(L[p - 1]);  // h
      {  // D9′: the whole steps that hold an unfinished call (serve: the owner's set; replay/eval: by the definition over [f, rs0))
        std::vector<size_t> ocp; std::vector<uint32_t> bl;
        for (uint32_t oi : sn.ocalls) if (S.it[oi].th <= t) { const size_t q = L.idx(sg, S.it[oi].pos); if (q >= f && q < rs0) ocp.push_back(q); }
        for (size_t q : ocp) {  // E2-56: the whole step of the response that holds the call
          size_t b = q; while (b > f && model(L[b - 1])) b--;
          step_of(b, stp); P.insert(P.end(), stp.begin(), stp.end());
          A.ocalls++;
        }
      }
      {  // L1 v23′ D10″ / L2 v24 E2-58: W(x, t), every step of the open turn, is required input (the turn's working set, §88)
         // VRS_클로드코드_v1 4판 (S.segwin): whole steps only from the step holding the second-latest progress report
         // (K_ASSIST in [f, hi)); before it, the progress reports alone, the SEG_W latest.
        size_t q2 = SIZE_MAX;
        if (S.segwin) { int seen = 0; for (size_t q = hi; q > f; q--) if (S.it[L[q - 1]].kind == K_ASSIST && ++seen == 2) { q2 = q - 1; break; } }
        size_t e = rs0; std::vector<uint32_t> st; size_t reps = 0;
        while (e > f) {
          size_t b = e; while (b > f && !model(L[b - 1])) b--;  // the step's tail after its response
          while (b > f && model(L[b - 1])) b--;                // its response
          if (q2 != SIZE_MAX && e <= q2) {                     // 4판: wholly before the window
            for (size_t p = e; p > b && reps < SEG_W; p--) if (S.it[L[p - 1]].kind == K_ASSIST) { P.push_back(L[p - 1]); reps++; A.seg_rep++; }
            if (reps >= SEG_W) break;
            e = b; continue;
          }
          step_of(b, st);                                      // E2-56: D8's step
          for (uint32_t z : st) A.trace_tok += S.it[z].tok;
          A.trace_in++;
          P.insert(P.end(), st.begin(), st.end());
          e = b;
        }
        A.segwin = q2 != SIZE_MAX;
      }
      // L1 v25 D5⁹: the reply context is no longer forced; the turn just before x is judged like any other element
      {  // L2 v25 E2-62 (R6-04): a chosen output brings its call; a chosen reasoning or call item brings its response run
        auto mdl = [&](uint32_t w) { const uint8_t k = S.it[w].kind; return k == K_REASON || k == K_ASSIST || k == K_FC || k == K_CTC; };
        std::unordered_set<uint32_t> in(P.begin(), P.end());
        std::vector<uint32_t> bl, add;
        auto put = [&](uint32_t z) { if (in.insert(z).second) { add.push_back(z); A.closure_add++; } };
        auto calls_of_outputs = [&](const std::vector<uint32_t>& src) {
          for (uint32_t z : src) { const uint8_t k = S.it[z].kind; if (k != K_FCO && k != K_CTCO) continue;
            block(z, t, bl); for (uint32_t c : bl) if (S.it[c].kind == K_FC || S.it[c].kind == K_CTC) put(c); } };
        auto runs_of = [&](const std::vector<uint32_t>& src) {
          for (uint32_t z : src) { const uint8_t k = S.it[z].kind; if (k != K_REASON && k != K_FC && k != K_CTC) continue;
            const size_t q = L.idx(S.it[z].sess, S.it[z].pos);
            if (q >= L.size() || L[q] != z) continue;
            for (size_t r = q; r > 0 && mdl(L[r - 1]) && thv(L[r - 1]) <= t && S.it[L[r - 1]].sess == S.it[z].sess; r--) put(L[r - 1]);
            for (size_t r = q + 1; r < hi && mdl(L[r]) && S.it[L[r]].sess == S.it[z].sess; r++) put(L[r]); } };
        auto outputs_of_calls = [&](const std::vector<uint32_t>& src) {
          for (uint32_t z : src) { const uint8_t k = S.it[z].kind; if (k != K_FC && k != K_CTC) continue;
            block(z, t, bl); for (uint32_t o : bl) put(o); } };
        add.clear(); calls_of_outputs(P); std::vector<uint32_t> s1 = add; P.insert(P.end(), s1.begin(), s1.end());
        add.clear(); runs_of(P); std::vector<uint32_t> s2 = add; P.insert(P.end(), s2.begin(), s2.end());
        add.clear(); outputs_of_calls(s2); P.insert(P.end(), add.begin(), add.end());
      }
      {  // D6⁶ I(t, Λ): serve = the owner's max-merge state, replay/eval = the scan of the definition
        std::vector<uint32_t> I;
        sn.ist->current(S.it, t, I);
        std::sort(I.begin(), I.end(), [&](uint32_t a, uint32_t b) { return S.it[a].th < S.it[b].th; });
        A.x0_n = uint32_t(P.size()); for (uint32_t m : P) A.x0_tok += S.it[m].tok;
        A.i_n = uint32_t(I.size()); for (uint32_t m : I) A.i_tok += S.it[m].tok;
        P.insert(P.end(), I.begin(), I.end());
      }
      used += take_prefix(P, t, B, taken);
      A.present_tok = used;
      for (uint32_t m : P) if (!taken.count(m)) { A.overflow = true; break; }
      if (A.overflow) { A.tokens = used; A.ids.clear(); return A; }  // E2-22: never a silently cut present input
      std::vector<uint64_t> Q0;
      const uint64_t th0 = open ? std::max<uint64_t>(sn.th0, Mb.back()) : t;  // D16
      if (open && sn.tq1 && t >= sn.tq1) Q0 = sn.q1;  // R04 (L3 v18 fix): Q₁, not the text accumulated by t
      // L1 v26 D5¹⁰ N₃ (3판 E3, VRS_정확입력_v1 §2): every direct utterance, the answers of the last W = 8 turns and of the K = 4
      // earlier turns most related to the present utterance (sigma = sum of ln(L / df) over shared cues); required, never cut
      std::unordered_set<uint32_t> n3t;
      // L3 v27: A.mem is a set; list it in a canonical order (n3t is an unordered_set, its walk order is not a meaning)
      auto canon_mem = [&](Asm& a) { std::sort(a.mem.begin(), a.mem.end(), [&](const auto& p, const auto& q) {
        return std::make_tuple(TT[p.first]->th0, TT[p.first]->ord0, p.second) < std::make_tuple(TT[q.first]->th0, TT[q.first]->ord0, q.second); }); };
      const uint64_t thx_open = open ? th0 : UINT64_MAX;  // the open turn's own utterances (a relay that opened x is its question)
      {
        std::vector<uint32_t> N3;
        for (size_t q = 0; q < uh; q++) if (!S.it[U[q]].fwd || (thx_open != UINT64_MAX && vpos(U[q]) >= from)) N3.push_back(U[q]);  // D17  // relays: with their turn below
        const size_t L0 = i0, W3 = 8, K3 = 4;
        for (size_t j = L0 > W3 ? L0 - W3 : 0; j < L0; j++) if (TT[TV[j]]->the <= t) n3t.insert(TV[j]);
        if (!Q0.empty() && L0 > W3) {
          std::unordered_map<uint64_t, uint32_t> df; std::vector<std::vector<uint64_t>> isec(L0 - W3);
          for (size_t j = 0; j < L0; j++) {
            const auto& Qj = TT[TV[j]]->Q; std::vector<uint64_t> o;
            std::set_intersection(Q0.begin(), Q0.end(), Qj.begin(), Qj.end(), std::back_inserter(o));
            for (uint64_t h : o) df[h]++;
            if (j < L0 - W3) isec[j] = std::move(o);
          }
          std::vector<std::pair<double, size_t>> sc;
          for (size_t j = 0; j < L0 - W3; j++) {
            if (TT[TV[j]]->the > t || isec[j].empty()) continue;
            double v = 0; for (uint64_t h : isec[j]) v += std::log(double(L0) / double(df[h]));
            if (v > 0) sc.push_back({v, j});
          }
          std::sort(sc.begin(), sc.end(), [&](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : TT[TV[a.second]]->th0 > TT[TV[b.second]]->th0; });
          for (size_t q = 0; q < sc.size() && q < K3; q++) n3t.insert(TV[sc[q].second]);
        }
        // L3 v28: walk the near turns in a canonical order (n3t is an unordered_set; take_prefix at E2-22 must not see its walk order)
        std::vector<uint32_t> n3v(n3t.begin(), n3t.end());
        std::sort(n3v.begin(), n3v.end(), [&](uint32_t p, uint32_t q) { return std::make_pair(TT[p]->th0, TT[p]->ord0) < std::make_pair(TT[q]->th0, TT[q]->ord0); });
        for (uint32_t y : n3v) { uint64_t c = 0; for (uint32_t z : vi_part(S, y, 1)) { N3.push_back(z); if (!taken.count(z)) c += S.it[z].tok; } A.mem.push_back({y, 1}); A.mem_tok += c; A.n3_ans++;
          for (uint32_t z : vi_part(S, y, 0)) if (S.it[z].fwd) N3.push_back(z); }
        const uint64_t u0 = used;
        used += take_prefix(N3, t, B - used, taken);
        A.n3_tok = used - u0;
        for (uint32_t m : N3) if (!taken.count(m)) { A.overflow = true; break; }
        if (A.overflow) { A.tokens = used; A.ids.clear(); canon_mem(A); return A; }  // E2-22
      }
      // L1 v27 D5¹¹ N_far: for each rare cue h of the present that no near turn covers (Y(h) ∩ T₃ = ∅), the approved content
      // elements (D5¹⁰ (i)-(iii)) on Y(h) of largest sigma, all ties; the union over h, no greedy cover (P29); B cuts by sigma
      // L1 v27 D5¹¹ N_far: for each rare cue h of the present that no near turn covers (Y(h) ∩ T₃ = ∅), the approved content
      // elements (D5¹⁰ (i)-(iii)) on Y(h) of largest sigma, all ties; the union over h, no greedy cover (P29); B cuts by sigma
      auto needs = need_list(S, TV, i0, t, cells, Q0, open ? th0 : t);
      {
        std::vector<std::pair<uint64_t, std::vector<uint32_t>>> RL;
        if (!Q0.empty()) RL = rare_lists(S, Q0, cuts_of(TV, i0), open ? th0 : t);
        const double Lc = double(std::max<size_t>(i0, 1));
        std::unordered_map<uint32_t, double> sig;  // sigma(x, y) = sum over the rare cues y shares with Q of ln(L / df)
        for (auto& [h, ys] : RL) for (uint32_t y : ys) sig[y] += std::log(Lc / double(ys.size()));
        std::unordered_map<uint32_t, std::vector<int>> ap;  // Ap(x): y -> its approved content parts
        for (auto& nd : needs) {  // L1 v33 D5¹⁶: the content key measures V(y) (request and final answer); only the answer part is backed by it
          if (!nd.bycontent || nd.part != 1) continue;
          if (n3t.count(nd.y)) continue;
          ap[nd.y].push_back(nd.part);
        }
        std::set<std::pair<uint32_t, int>> far;
        A.far_rc = uint32_t(RL.size());
        for (auto& [h, ys] : RL) {
          bool cov = false;
          for (uint32_t y : ys) if (n3t.count(y)) { cov = true; break; }
          if (cov) continue;
          A.far_uc++;
          double mx = -1;
          for (uint32_t y : ys) if (ap.count(y)) mx = std::max(mx, sig[y]);
          if (mx < 0) continue;
          for (uint32_t y : ys) if (ap.count(y) && sig[y] == mx) for (int pt : ap[y]) far.insert({y, pt});
        }
        std::vector<std::pair<double, std::pair<uint32_t, int>>> fv;
        for (auto& e : far) fv.push_back({sig[e.first], e});
        // B5: sigma down, then the turn by (its file's store name, ord0), then the part; never the turn id (an arrival number)
        std::sort(fv.begin(), fv.end(), [&](const auto& a, const auto& b) {
          if (a.first != b.first) return a.first > b.first;
          const uint32_t ya = a.second.first, yb = b.second.first;
          if (ya != yb) {
            const TurnRec& x = *S.TT[ya]; const TurnRec& y = *S.TT[yb];
            if (x.sg != y.sg) { const int c = vp_path(S.sx(x.sg).nu).compare(vp_path(S.sx(y.sg).nu)); if (c != 0) return c < 0; }
            if (x.ord0 != y.ord0) return x.ord0 < y.ord0;
          }
          return a.second.second < b.second.second;
        });
        for (auto& [sv, e] : fv) {
          const auto items = vi_part(S, e.first, e.second);
          uint64_t sz = 0; for (uint32_t z : items) if (!taken.count(z)) sz += S.it[z].tok;
          if (used + sz > B) break;
          used += sz; A.mem_tok += sz; A.mem.push_back({e.first, e.second}); A.far_n++; A.far_tok += sz;
          for (uint32_t z : items) taken.insert(z);
        }
      }
      A.ids.assign(taken.begin(), taken.end());
      std::sort(A.ids.begin(), A.ids.end(), oless);  // D17
      canon_mem(A);
      A.tokens = used; A.c3_na = true;
      return A;
    }
    return A;
  }
};

std::string payload_of(const std::string& line) {  // the "payload" object, verbatim
  size_t i = line.find("\"payload\":");
  if (i == std::string::npos) return "null";
  i += 10;
  size_t j = i; int depth = 0; size_t n = line.size();
  for (; j < n; j++) {
    char c = line[j];
    if (c == '"') { j++; while (j < n && line[j] != '"') j += line[j] == '\\' ? 2 : 1; continue; }
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') { if (--depth == 0) { j++; break; } }
  }
  return line.substr(i, j - i);
}

// ---------------- the index shard (P-1) ----------------
// One append-only file per rollout file: <index_dir>/<relative path>.vx. A record is  magic u32 | type u8 | pad 3 | len u32 | chk u32 | payload.
// Every record carries its own key (byte offsets inside the rollout file), so the reader keeps one record per key and the same record
// appended twice (by this process and by another) changes nothing (T-int-4). A torn or damaged record fails its check and is skipped.
constexpr uint32_t VX_MAGIC = 0x36305856u;  // "VX06" (6th stage: D12 seeds); before: "VX05", little endian (5th stage: `rho` of the records is the ordinal, a unit is keyed by the fingerprint of the set it was counted over)
enum : uint8_t { R_ITEM = 1, R_META = 2, R_TS = 3, R_TURN = 4, R_CONTRIB = 5, R_WM = 6, R_CONTRIB2 = 7, R_K5U = 8, R_K5C = 9, R_K5C2 = 10 };  // R_K5U / R_K5C: the K5 units (B-r7); R_K5C2: the 4판 K5C (§17 B-s4; R_K5C is read and not used)  // R_CONTRIB (5) has no fingerprint: read and not used (P-8)  // R_WM: every line before this offset is on disk (P-3')

uint32_t chk32(const void* p, size_t n) { const uint64_t h = fnv((const char*)p, n); return uint32_t(h ^ (h >> 32)); }
void put_u(std::string& b, uint64_t v, int n) { for (int i = 0; i < n; i++) b.push_back(char((v >> (8 * i)) & 0xff)); }
struct Wr {
  std::string b;
  void u8(uint64_t v) { put_u(b, v, 1); }
  void u16(uint64_t v) { put_u(b, v, 2); }
  void u32(uint64_t v) { put_u(b, v, 4); }
  void u64(uint64_t v) { put_u(b, v, 8); }
  void str(const std::string& s) { u16(s.size() > 65535 ? 65535 : s.size()); b.append(s, 0, 65535); }
  void vec64(const std::vector<uint64_t>& v) { u32(v.size()); for (uint64_t x : v) u64(x); }
  void vec32(const std::vector<uint32_t>& v) { u32(v.size()); for (uint32_t x : v) u32(x); }
};
void frame(std::string& out, uint8_t type, const std::string& pl) {
  put_u(out, VX_MAGIC, 4); out.push_back(char(type)); out.append(3, '\0'); put_u(out, pl.size(), 4); put_u(out, chk32(pl.data(), pl.size()), 4); out += pl;
}
struct Rd {
  const uint8_t* p; const uint8_t* e; bool ok = true;
  uint64_t get(int n) { if (e - p < n) { ok = false; p = e; return 0; } uint64_t v = 0; for (int i = 0; i < n; i++) v |= uint64_t(p[i]) << (8 * i); p += n; return v; }
  uint64_t u8() { return get(1); }
  uint64_t u16() { return get(2); }
  uint64_t u32() { return get(4); }
  uint64_t u64() { return get(8); }
  std::string str() { const size_t n = u16(); if (size_t(e - p) < n) { ok = false; p = e; return {}; } std::string s((const char*)p, n); p += n; return s; }
  bool vec64(std::vector<uint64_t>& v) { const uint64_t n = u32(); if (!ok || n > size_t(e - p) / 8) { ok = false; p = e; return false; } v.resize(n); for (auto& x : v) x = u64(); return ok; }
  bool vec32(std::vector<uint32_t>& v) { const uint64_t n = u32(); if (!ok || n > size_t(e - p) / 4) { ok = false; p = e; return false; } v.resize(n); for (auto& x : v) x = (uint32_t)u32(); return ok; }
};

// What a record decodes to (before it becomes a table entry).
struct ItemP { uint64_t off = 0, ts = 0, cid = 0, ch = 0, rs = 0; uint32_t len = 0, rho = 0, ord = 0, pos = 0, tok = 0, pfx = 0, r = 0; uint8_t kind = 0, flags = 0, mode = 0; std::vector<Item::Part> parts; };
struct MetaP { uint64_t off = 0, ts = 0, fk = 0; uint32_t len = 0, rho = 0, r = 0; std::string id, fpar; };
struct TsP { uint64_t off = 0, ts = 0; uint32_t len = 0, rho = 0, ord = 0, r = 0; };
struct TurnP {
  uint64_t o0 = 0, o1 = 0, th0 = 0, the = 0, tq1 = 0; uint32_t ord0 = 0, ord1 = 0, pos0 = 0, pos1 = 0, n = 0, src = 0, nu = 0;
  std::vector<uint64_t> vi, C, D, Q, Q1, Nc; std::vector<uint32_t> len, viidx;
};
struct ContribP { uint64_t o0 = 0, ny = 0, phi = 0; uint32_t root = 0; std::vector<CEnt> e; };  // ny: size of Y; phi: the sum of the window fingerprints of Y
struct Shard {
  std::vector<ItemP> items; std::vector<TsP> tss; std::vector<MetaP> metas; std::vector<TurnP> turns; std::unordered_map<uint64_t, std::vector<ContribP>> contribs; uint64_t legacy_contribs = 0;
  bool present = false, current = true; uint64_t corrupt = 0;
  bool has_wm = false; uint64_t wm = 0;  // the largest R_WM; without one the shard is from a library that only recorded lines it had read from the file
  std::unordered_map<uint64_t, std::vector<std::pair<uint8_t, std::string>>> k5rec;  // K5 units by o0 (raw payloads)
};

void emit_item(std::string& out, const Item& m, uint64_t ts, uint32_t r, uint32_t rho, uint32_t pfx) {
  Wr w;
  w.u64(m.off); w.u32(m.len); w.u32(rho); w.u32(m.ord); w.u32(m.pos); w.u8(m.kind); w.u8(m.fwd ? 1 : 0); w.u8(m.mode); w.u16(m.parts.size());
  w.u64(ts); w.u32(r); w.u32(m.tok); w.u32(pfx); w.u64(m.cid); w.u64(m.ch); w.u64(m.rs);
  for (const auto& p : m.parts) { w.u64(p.subj); w.u8(p.mode); w.u8(p.unit); w.u32(p.keys.size()); for (uint64_t k : p.keys) w.u64(k); }
  frame(out, R_ITEM, w.b);
}
void emit_meta(std::string& out, uint64_t off, uint32_t len, uint32_t rho, uint64_t ts, uint32_t r, const Sess& ss) {
  Wr w; w.u64(off); w.u32(len); w.u32(rho); w.u64(ts); w.u32(r); w.u64(ss.fk); w.str(ss.id); w.str(ss.fpar);
  frame(out, R_META, w.b);
}
void emit_ts(std::string& out, uint64_t off, uint32_t len, uint32_t rho, uint32_t ord, uint64_t ts, uint32_t r) {
  Wr w; w.u64(off); w.u32(len); w.u32(rho); w.u32(ord); w.u64(ts); w.u32(r);
  frame(out, R_TS, w.b);
}
// the cells a counted turn added: (cell, alpha, beta) -> n identical cell adds, as entries in cell order
Contrib* make_contrib(const std::vector<std::array<uint32_t, 3>>& rec, uint32_t root, uint64_t phi) {
  std::vector<uint32_t> cnt((size_t)NCA * 4, 0);
  for (const auto& e : rec) cnt[(size_t)e[0] * 4 + (e[1] & 3)]++;
  auto* c = new Contrib; c->phi = phi; c->root = root;
  for (size_t k = 0; k < cnt.size(); k++) if (cnt[k]) c->e.push_back(CEnt{(uint16_t)(k >> 2), (uint8_t)(k & 3), 0, cnt[k]});
  return c;
}
// R_CONTRIB2: o0 | root | |Y| | phi | n | n x (cell u16, flags u8, 0, count u32). A turn may have several: one per set it was counted over; restore takes the one whose
// fingerprint is the fingerprint of the set it finds (D11: Y is a function of the lineage, phi of the windows of Y).
void emit_contrib2(std::string& out, uint64_t o0, const Contrib& c) {
  Wr w; w.u64(o0); w.u32(c.root); w.u64(c.Y.size()); w.u64(c.phi); w.u32(c.e.size());
  for (const CEnt& e : c.e) { w.u16(e.cell); w.u8(e.fl); w.u8(0); w.u32(e.cnt); }
  frame(out, R_CONTRIB2, w.b);
}

void emit_wm(std::string& out, uint64_t wm) { Wr w; w.u64(wm); frame(out, R_WM, w.b); }
void decode_record(uint8_t type, const uint8_t* p, size_t n, Shard& sh) {
  Rd r{p, p + n};
  switch (type) {
    case R_WM: { const uint64_t v = r.u64(); if (r.ok) { sh.has_wm = true; if (v > sh.wm) sh.wm = v; } else sh.corrupt++; break; }
    case R_ITEM: {
      ItemP x;
      x.off = r.u64(); x.len = (uint32_t)r.u32(); x.rho = (uint32_t)r.u32(); x.ord = (uint32_t)r.u32(); x.pos = (uint32_t)r.u32();
      x.kind = (uint8_t)r.u8(); x.flags = (uint8_t)r.u8(); x.mode = (uint8_t)r.u8(); const size_t np = r.u16();
      x.ts = r.u64(); x.r = (uint32_t)r.u32(); x.tok = (uint32_t)r.u32(); x.pfx = (uint32_t)r.u32(); x.cid = r.u64(); x.ch = r.u64(); x.rs = r.u64();
      if (!r.ok || np > size_t(r.e - r.p) / 14) { sh.corrupt++; break; }
      x.parts.reserve(np);  // the record already gives the number of parts
      for (size_t i = 0; i < np && r.ok; i++) {
        Item::Part pt; pt.subj = r.u64(); pt.mode = (uint8_t)r.u8(); pt.unit = (uint8_t)r.u8(); const uint64_t nk = r.u32();
        if (!r.ok || nk > size_t(r.e - r.p) / 8) { r.ok = false; break; }
        pt.keys.resize(nk); for (auto& k : pt.keys) k = r.u64();
        x.parts.push_back(std::move(pt));
      }
      if (r.ok) sh.items.push_back(std::move(x)); else sh.corrupt++;
      break;
    }
    case R_META: {
      MetaP x; x.off = r.u64(); x.len = (uint32_t)r.u32(); x.rho = (uint32_t)r.u32(); x.ts = r.u64(); x.r = (uint32_t)r.u32(); x.fk = r.u64(); x.id = r.str(); x.fpar = r.str();
      if (r.ok) sh.metas.push_back(std::move(x)); else sh.corrupt++;
      break;
    }
    case R_TS: {
      TsP x; x.off = r.u64(); x.len = (uint32_t)r.u32(); x.rho = (uint32_t)r.u32(); x.ord = (uint32_t)r.u32(); x.ts = r.u64(); x.r = (uint32_t)r.u32();
      if (r.ok) sh.tss.push_back(x); else sh.corrupt++;
      break;
    }
    case R_TURN: {
      TurnP x;
      x.o0 = r.u64(); x.o1 = r.u64(); x.ord0 = (uint32_t)r.u32(); x.ord1 = (uint32_t)r.u32(); x.pos0 = (uint32_t)r.u32(); x.pos1 = (uint32_t)r.u32();
      x.th0 = r.u64(); x.the = r.u64(); x.tq1 = r.u64(); x.n = (uint32_t)r.u32(); x.src = (uint32_t)r.u32(); x.nu = (uint32_t)r.u32();
      r.vec64(x.vi); r.vec64(x.C); r.vec64(x.D); r.vec32(x.len); r.vec64(x.Q); r.vec64(x.Q1); r.vec64(x.Nc);
      if (r.ok) sh.turns.push_back(std::move(x)); else sh.corrupt++;
      break;
    }
    case R_CONTRIB: sh.legacy_contribs++; break;  // made before fingerprints: nothing says which set it was counted over, so the turn is counted again
    case R_CONTRIB2: {
      ContribP x; x.o0 = r.u64(); x.root = (uint32_t)r.u32(); x.ny = r.u64(); x.phi = r.u64(); const uint64_t ne = r.u32();
      if (!r.ok || ne > size_t(r.e - r.p) / 8) { sh.corrupt++; break; }
      x.e.reserve(ne);
      for (uint64_t i = 0; i < ne; i++) { const uint32_t cell = (uint32_t)r.u16(), fl = (uint32_t)r.u8(); r.u8(); const uint32_t c = (uint32_t)r.u32(); if (cell < (uint32_t)NCA) x.e.push_back(CEnt{(uint16_t)cell, (uint8_t)fl, 0, c}); }
      if (r.ok) { auto& v = sh.contribs[x.o0]; v.push_back(std::move(x)); } else sh.corrupt++;
      break;
    }
    case R_K5U: case R_K5C: case R_K5C2: {
      const uint64_t o0 = r.u64();
      if (r.ok) sh.k5rec[o0].push_back({type, std::string((const char*)p, n)}); else sh.corrupt++;
      break;
    }
    default: sh.corrupt++;
  }
}
void parse_shard(const uint8_t* base, size_t n, Shard& sh) {
  size_t pos = 0;
  while (pos + 16 <= n) {
    uint32_t magic, len, chk;
    std::memcpy(&magic, base + pos, 4);
    if (magic == VX_MAGIC) {
      std::memcpy(&len, base + pos + 8, 4); std::memcpy(&chk, base + pos + 12, 4);
      if (len <= n - pos - 16 && chk32(base + pos + 16, len) == chk) { decode_record(base[pos + 4], base + pos + 16, len, sh); pos += 16 + len; continue; }
    }
    sh.corrupt++;  // damaged or torn: resynchronise on the next magic
    size_t q = pos + 1;
    while (q + 4 <= n) { if (std::memcmp(base + q, &VX_MAGIC, 4) == 0) break; q++; }
    if (q + 4 > n) break;
    pos = q;
  }
}

std::string shard_path(const Store& S, const Sess& ss) { return S.idir + "/" + ss.nu + ".vx"; }  // B2: the store name
VrsFdLease shard_fd(Store& S, Sess& ss) {
  if (S.idir.empty() || ss.memmode) return {};
  return VrsHandleCache<VrsFd>::local().get(ss.shard_key, [&]() -> VrsFdLease {
    const std::string p = shard_path(S, ss);
    std::error_code ec;
    fs::create_directories(vp_io_path(p).parent_path(), ec);
    auto file = std::make_shared<VrsFd>(vp_open_append(p.c_str()));
    if (file->fd < 0) { S.shard_err.fetch_add(1); return {}; }
    // Every independent opener establishes P-3' before using its own handle.
    // Duplicate zero watermarks leave the existing max reduction unchanged.
    std::string h; emit_wm(h, 0);
    if (vp_append(file->fd, h.data(), h.size()) != (ssize_t)h.size()) { S.shard_err.fetch_add(1); return {}; }
    ss.shard_started.store(true, std::memory_order_release);
    return file;
  });
}
bool shard_write(Store& S, Sess& ss, const std::string& b) {  // one O_APPEND write per call: the records of the call stay together
  const auto file = shard_fd(S, ss);
  if (!file) return false;
  const int fd = file->fd;
  size_t w = 0;
  while (w < b.size()) { const ssize_t k = vp_append(fd, b.data() + w, b.size() - w); if (k <= 0) { S.shard_err.fetch_add(1); return false; } w += (size_t)k; }
  return true;
}
// P-3': the records of a run are written with one write; the mark up to which every line is on disk (the end of the processed prefix, or the first line still held
// in memory) is written when the processed prefix moves and when vrs_written lets go of a copy (the largest mark counts, so the order of the writers does not matter)
void wflush(Store& S, Sess& ss, std::string& buf) {
  if (buf.empty()) return;
  shard_write(S, ss, buf); buf.clear();
}
void mark_wm(Store& S, Sess& ss) {
  if (ss.memmode || !ss.shard_started.load(std::memory_order_acquire)) return;
  const uint64_t wm = std::min<uint64_t>(ss.pub.load(std::memory_order_acquire), uw_min(S, ss));
  uint64_t o = ss.wm_pub.load(std::memory_order_relaxed);
  while (wm > o && !ss.wm_pub.compare_exchange_weak(o, wm)) {}
  if (wm > o) { std::string b; emit_wm(b, wm); shard_write(S, ss, b); }
}

// ---------------- lines: records, maps, the prefix (P-11) ----------------
constexpr uint32_t CLAIMING = UINT32_MAX;  // lmap value while the thread that won the offset fills the record in
inline uint64_t lk(uint32_t sg, uint64_t off) { return (uint64_t(sg) << 40) | off; }  // offsets are below 2^40
// the record of the line that starts at off, once it is announced
LineRec* line_at(const Store& S, uint32_t sg, uint64_t off) {
  const auto* nd = S.lmap.find(lk(sg, off));
  if (!nd) return nullptr;
  const uint32_t v = nd->val.load(std::memory_order_seq_cst);
  return v && v != CLAIMING ? &S.lr[v] : nullptr;
}
// the line that ends (newline included) at `end`
LineRec* line_ending(const Store& S, uint32_t sg, uint64_t end) {
  const auto* nd = S.lend.find(lk(sg, end));
  if (!nd) return nullptr;
  const uint32_t v = nd->val.load(std::memory_order_acquire);
  return v ? &S.lr[v] : nullptr;
}
// a NUL-only hole that starts at off (restore): where the next line starts, 0 = none
uint64_t hole_after(const Store& S, uint32_t sg, uint64_t off) {
  const auto* nd = S.holes.find(lk(sg, off));
  return nd ? nd->val.load(std::memory_order_acquire) : 0;
}
void link_call(Store& S, uint32_t sg, uint32_t id) {  // the group of (session, call_id) is a stack; readers give it the order of the lines
  Item& m = S.it[id];
  CallVal& g = S.calls.get(call_key(sg, m.cid))->val;
  uint32_t h = g.head.load(std::memory_order_relaxed);
  do { m.nxt.store(h, std::memory_order_relaxed); } while (!g.head.compare_exchange_weak(h, id, std::memory_order_release, std::memory_order_relaxed));
}
void deps_add(Store& S, uint32_t y, uint32_t x) {  // x holds y among its Y: when the window of y changes, x is looked at again
  TurnRec& t = *S.TT[y];
  for (;;) {
    DepChunk* h = t.deps.load(std::memory_order_seq_cst);
    if (h) {
      const uint32_t k = h->n.fetch_add(1, std::memory_order_seq_cst);
      if (k < 14) { h->id[k].store(x, std::memory_order_seq_cst); return; }
    }
    DepChunk* nc = new DepChunk; nc->n.store(1); nc->id[0].store(x); nc->next.store(h);
    if (t.deps.compare_exchange_strong(h, nc, std::memory_order_seq_cst)) return;
    delete nc;
  }
}

// D2 + observer v11 turn data of a closed turn. fill_turn_rec makes the data (nothing is published); finalize_turn publishes it in TT and the session's
// turn list. libvrs: no capacity errors (the tables grow).
void fill_turn_rec(Store& S, Sess& ss, TurnParse& p, TurnRec* t, uint32_t pos1, uint64_t the, uint32_t ord1, uint32_t tpos) {
  const std::string user = req_text(p);
  const std::string fin = p.final_msg.empty() ? p.last_assistant : p.final_msg;
  const std::string uq = clip_chars(user, 1500), aq = clip_chars(fin, 2500);
  { const auto A = cue_set(aq); t->Q = cue_set(uq); std::set_difference(A.begin(), A.end(), t->Q.begin(), t->Q.end(), std::back_inserter(t->C)); }
  bool part_seen[2] = {false, false};
  std::unordered_set<uint64_t> seen;
  for (int part = 0; part < 2; ++part) {
    const std::string& s = part == 0 ? uq : aq;
    for (size_t b = 0;;) {
      const size_t e = s.find('\n', b);
      const auto k = cue_set(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
      size_t added = 0;
      for (auto h : k) if (seen.insert(h).second) { t->D.push_back(h); ++added; }
      if (added) { t->len.push_back(uint32_t(added)); part_seen[part] = true; if (part == 0) t->nu++; }
      if (e == std::string::npos) break;
      b = e + 1;
    }
  }
  t->n = uint32_t(t->len.size()); t->src = uint32_t(part_seen[0]) + uint32_t(part_seen[1]);
  t->sg = ss.sg; t->ord0 = p.ord0; t->ord1 = ord1; t->pos0 = p.pos0; t->pos1 = pos1;
  t->th0 = p.th0; t->the = the;
  t->seed = ident_hash(ss) ^ (uint64_t(p.ord0) * 0x9E3779B97F4A7C15ull);  // D12
  t->vi = p.req;  // D2⁗ (E2-33): VI^u = utterances, VI^n = the event items of the turn, then the final answer
  t->vi.insert(t->vi.end(), p.loose.begin(), p.loose.end());
  t->Nc = cue_set(clip_chars(p.ntext, 2500));  // D3‴
  if (p.fin != UINT32_MAX) t->vi.push_back(p.fin);
  t->Q1 = p.q1; t->tq1 = p.tq1;
  t->tpos = tpos;
  t->o0 = p.o0;
}

// ---------------- D8 (VRS_코덱스일체화_v1.md §5.6): the turn is a function of the set of its lines ----------------
// What a line leaves for its turn (the old agg_feed, made once when the line is handed in; nothing in it depends on the other lines). nullptr = nothing but
// its ordinal (world_state lines, events, and response items that are none of the kinds below).
TD* make_td(const Store& S, const std::string& line, uint32_t item) {
  TD* t = nullptr;
  auto get = [&]() -> TD& { if (!t) t = new TD; return *t; };
  const auto ty = find_field(line, "\"type\":\"", 120);
  if (ty == "event_msg") {
    if (find_field(line, "\"payload\":{\"type\":\"", 200) == "task_complete") {
      const auto at = line.find("\"last_agent_message\":\"");
      if (at != std::string::npos) { TD& d = get(); d.f |= TF_FINAL; d.text = json_string(line, at + 22); }
    }
    return t;
  }
  if (ty != "response_item" || !item) return t;
  const Item& m = S.it[item];
  if (m.kind == K_DEV && m.mode == MD_EVENT) {  // E2-33: VI^n = the event items
    TD& d = get(); d.f |= TF_LO;
    for (size_t at = line.find("\"text\":\""); at != std::string::npos; at = line.find("\"text\":\"", at + 8)) { d.nt += json_string(line, at + 8); d.nt += '\n'; }
  }
  if ((m.kind == K_FC || m.kind == K_CTC || m.kind == K_FCO || m.kind == K_CTCO) && m.cid) get().f |= (m.kind == K_FC || m.kind == K_CTC) ? TF_CALL : TF_OUT;  // D9′: unfinished calls
  const auto pt = find_field(line, "\"payload\":{\"type\":\"", 200);
  const auto role = find_field(line, "\"role\":\"", 400);
  const bool asst = pt == "message" && role == "assistant";
  const bool user = pt == "message" && role == "user";
  const bool agent = pt == "agent_message";
  if (!asst && !user && !agent) { if (pt != "message") get().f |= TF_WORK; return t; }  // a working line (neither message nor agent_message): W = min o
  std::string texts;
  for (size_t at = line.find("\"text\":\""); at != std::string::npos; at = line.find("\"text\":\"", at + 8)) {
    const auto tx = json_string(line, at + 8);
    if (!asst && (tx.rfind("<environment_context>", 0) == 0 || tx.rfind("<external_codex_apps", 0) == 0)) continue;
    texts += tx; texts += '\n';
  }
  TD& d = get();
  if (asst) { d.f |= TF_WORK | TF_ASST; d.text = std::move(texts); }  // an assistant message is a working line
  else { d.f |= TF_IN | (agent ? TF_AGENT : 0); d.text = std::move(texts); }
  return t;
}
// Adding a line to the aggregate changes it by min / max / exists / o-sorted insertion alone: the turn does not depend on the order the lines are added in.
void agg_add(const Store& S, TurnAgg& g, const LineRec& L, const TD* d) {
  if (L.ord != UINT32_MAX && L.ord > g.ordl) g.ordl = L.ord;
  if (!d) return;
  const uint64_t off = L.off; const uint32_t item = L.item;
  if (d->f & TF_LO) { AgLo l; l.item = item; l.nt = d->nt; g.lo[off] = std::move(l); }
  if (d->f & (TF_CALL | TF_OUT)) { const uint64_t cid = S.it[item].cid; if (d->f & TF_CALL) g.calls.emplace(cid, item); else g.outs.insert(cid); }
  if ((d->f & TF_WORK) && off < g.W) g.W = off;
  if (d->f & TF_ASST) { AgAs a; a.text = d->text; a.item = item; g.as[off] = std::move(a); }
  if (d->f & TF_IN) {
    AgIn c; c.item = item; c.kind = S.it[item].kind; c.th = S.it[item].th; c.text = d->text; c.agent = (d->f & TF_AGENT) != 0;
    g.in[off] = std::move(c);
  }
}
// the D8 fields of the turn, from the lines added (the open turn: up to the end of the processed prefix; a closed one: all of them); final_msg is the caller's
void agg_parse(const TurnAgg& g, TurnParse& p) {
  p = TurnParse{};
  p.ord0 = g.ord0; p.ordl = g.ordl; p.pos0 = g.pos0; p.th0 = g.th0; p.o0 = g.o0;
  for (const auto& kv : g.as) {  // the assistant side: first non-empty text, last text, last item
    if (p.first_assistant.empty()) p.first_assistant = kv.second.text;
    p.last_assistant = kv.second.text;
    if (kv.second.item != UINT32_MAX) p.fin = kv.second.item;
  }
  for (const auto& kv : g.in) {  // the input lines in o order: user messages, and agent_messages before the first working line
    const uint64_t o = kv.first; const AgIn& c = kv.second;
    if (c.agent && o > g.W) continue;
    p.user += c.text;
    if (c.item != UINT32_MAX && c.kind == K_USER) {
      if (p.req.empty()) {  // Q1: the request text of the input lines up to the first request (R04)
        TurnParse tmp; tmp.user = p.user;
        for (const auto& a : g.as) { if (a.first >= o) break; if (!a.second.text.empty()) { tmp.first_assistant = a.second.text; break; } }
        p.q1 = cue_set(clip_chars(req_text(tmp), 1500)); p.tq1 = c.th;
      }
      p.req.push_back(c.item);
    }
  }
  for (const auto& kv : g.lo) { p.loose.push_back(kv.second.item); p.ntext += kv.second.nt; }
}
std::vector<uint32_t> agg_ocalls_set(const TurnAgg& g) {  // the set form: calls whose call_id has no output in the turn, by item id
  std::vector<uint32_t> v;
  for (const auto& kv : g.calls) if (!g.outs.count(kv.first)) v.push_back(kv.second);
  std::sort(v.begin(), v.end());
  return v;
}

void drive_counts(Store& S, Sess& ss);
void on_turn_in(Store& S, uint32_t z);
void k5_turn_in(Store& S, uint32_t z);    // vrs_k5.cpp
void k5_start(Store& S);
void k5_maybe_ready(Store& S);
void k5_run_task(Store& S, uint64_t p);
void k5_attach(TurnRec& t, const std::vector<std::pair<uint8_t, std::string>>& recs);
struct K5State* k5_new();
bool ensure_segs(Store& S, Sess& ss, bool force);
void wake_waiters(Store& S, Sess& ss);
void maybe_ready(Store& S);
void claim_and_run(Store& S, Sess& ss);
bool insert_line(Store& S, Sess& ss, uint64_t off, const char* p, size_t len, bool claim);

// ---------------- turn slots: the lines of a turn, its end, its closing (P-7) ----------------
void lower_E(TurnSlot& sl, uint64_t e) {  // the end of a turn only goes down: the first task_complete in its slot, or the next start
  uint64_t c = sl.E.load(std::memory_order_seq_cst);
  while (e < c && !sl.E.compare_exchange_weak(c, e, std::memory_order_seq_cst)) {}
}
void push_line(Store& S, TurnSlot& sl, uint32_t lid) {
  LineRec& L = S.lr[lid];
  uint32_t h = sl.lst.load(std::memory_order_relaxed);
  do { L.nextl = h; } while (!sl.lst.compare_exchange_weak(h, lid, std::memory_order_seq_cst, std::memory_order_relaxed));
  sl.tot.fetch_add(uint64_t(L.len) + 1 + L.gap, std::memory_order_seq_cst);
}
// the turn of slot m is closed when its end is known, its start line is processed and its lines cover [o0, end) byte for byte (P-7). The covering is checked by a
// walk over the lines of the slot, but only when the byte total says it may be complete, so the walk is made about once per turn.
void finalize_turn(Store& S, Sess& ss, uint32_t m, uint64_t e);
bool try_close(Store& S, Sess& ss, uint32_t m) {
  TurnSlot& sl = ss.slots.at(m);
  if (sl.closed.load(std::memory_order_seq_cst)) return false;
  const uint64_t e = sl.E.load(std::memory_order_seq_cst);
  if (e == UINT64_MAX || !sl.start_ready.load(std::memory_order_seq_cst)) return false;
  const uint64_t o0 = sl.o0.load(std::memory_order_acquire), Eo = e >> 1;
  if (sl.tot.load(std::memory_order_seq_cst) < Eo - o0) return false;
  uint64_t sum = 0;
  for (uint32_t id = sl.lst.load(std::memory_order_seq_cst); id; id = S.lr[id].nextl) {
    const LineRec& L = S.lr[id];
    if (L.off < Eo && L.off >= o0) sum += uint64_t(L.len) + 1 + (L.off > o0 ? L.gap : 0);
  }
  if (const LineRec* EL = line_at(S, ss.sg, Eo)) sum += EL->gap;  // a hole right before the end line is inside the turn
  if (sum != Eo - o0) return false;
  if (sl.E.load(std::memory_order_seq_cst) != e) return false;  // lowered meanwhile: the thread that lowered it tries again
  uint8_t z = 0;
  if (!sl.closed.compare_exchange_strong(z, 1, std::memory_order_seq_cst)) return false;
  finalize_turn(S, ss, m, e);
  return true;
}
void finalize_turn(Store& S, Sess& ss, uint32_t m, uint64_t e) {
  EbrGuard guard(S);
  TurnSlot& sl = ss.slots.at(m);
  const bool by_start = (e & 1) != 0; const uint64_t Eo = e >> 1;
  const LineRec* EL = line_at(S, ss.sg, Eo);  // the end line (the next start, or the task_complete)
  TurnAgg g; g.o0 = sl.o0.load(); g.th0 = sl.th0.load(); g.ord0 = sl.ord0.load(); g.pos0 = sl.pos0.load(); g.ordl = g.ord0;
  for (uint32_t id = sl.lst.load(std::memory_order_seq_cst); id; id = S.lr[id].nextl) {
    const LineRec& L = S.lr[id];
    if (L.off >= g.o0 && L.off < Eo) agg_add(S, g, L, L.td.load(std::memory_order_acquire));
  }
  TurnParse p; agg_parse(g, p);
  if (!by_start && EL) { const TD* d = EL->td.load(std::memory_order_acquire); if (d && (d->f & TF_FINAL)) p.final_msg = d->text; }
  // E2-52: a turn cut without task_complete ends where the next starts: its end is θ* of that line, its last ordinal the largest in the turn
  const uint32_t ord1 = by_start ? g.ordl : EL->ord;
  const uint64_t the = EL->th;
  const uint32_t pos1 = EL->ia - ((EL->fl & LF_ITEM) ? 1u : 0u);
  const uint32_t id = S.nturn.fetch_add(1) + 1;
  TurnRec* t = &S.TT.v.ensure(id);
  fill_turn_rec(S, ss, p, t, pos1, the, ord1, m - 1);
  t->o1 = Eo;
  for (uint32_t i = sl.lst.load(std::memory_order_seq_cst); i; i = S.lr[i].nextl)  // the data of the lines is let go (readers of the open turn may still hold it)
    if (TD* d = S.lr[i].td.exchange(nullptr, std::memory_order_acq_rel)) retire_obj(S, d, 4, sizeof(TD) + d->nt.size() + d->text.size());
  for (uint64_t h : t->D) S.pt_add(ss.sg, h, t->tpos);  // before the turn is in the list: readers walk the cue lists below a published cut
  S.pend_total.fetch_add(1, std::memory_order_seq_cst);  // the turn is not counted yet
  if (!ss.memmode) S.B.insert(id, t->the);  // D3′: the turn is in 𝔅 from the moment it closes (a thread without a file is not a file of the store)
  if (!ss.memmode) {
    std::string rb;
    Wr w;
    w.u64(t->o0); w.u64(Eo); w.u32(t->ord0); w.u32(t->ord1); w.u32(t->pos0); w.u32(t->pos1); w.u64(t->th0); w.u64(t->the); w.u64(t->tq1);
    w.u32(t->n); w.u32(t->src); w.u32(t->nu);
    w.u32(t->vi.size()); for (uint32_t z : t->vi) w.u64(S.it[z].off);
    w.vec64(t->C); w.vec64(t->D); w.vec32(t->len); w.vec64(t->Q); w.vec64(t->Q1); w.vec64(t->Nc);
    frame(rb, R_TURN, w.b);
    shard_write(S, ss, rb);
  }
  ss.tl.set(m - 1, id);
  on_turn_in(S, id);  // P-10
  if (!ss.memmode) k5_turn_in(S, id);  // K5 B-r6
  if (ensure_segs(S, ss, false)) drive_counts(S, ss);  // (ensure_segs counts by itself when it sets the lineage up)
}

// ---------------- processing a line (P-11): everything it needs is a count over the lines before it ----------------
// D2: θ* of a line = its timestamp + the number of lines of its (file, ms) group before it. P1 (ts never decreases along a file) makes the group the run of lines with
// the same ts right before the line; a group that began before the restored prefix is carried by `base`.
uint32_t group_rank(const Store& S, const Sess& ss, const LineRec& L, bool* decreasing = nullptr) {
  uint32_t r = 0; uint64_t cur = L.off;
  for (;;) {
    const LineRec* P = line_ending(S, ss.sg, cur);
    if (!P) {
      if (cur == ss.base_end && ss.base.has) { if (ss.base.ts == L.ts) r += ss.base.r + 1; else if (ss.base.ts > L.ts && decreasing) *decreasing = true; }
      break;
    }
    if (P->ts != L.ts) { if (P->ts > L.ts && decreasing) *decreasing = true; break; }
    if (P->done.load(std::memory_order_acquire)) { r += P->r + 1; break; }  // processed: its rank is known
    r++; cur = P->off;
  }
  return r;
}
uint64_t item_theta(Store& S, const Sess& ss, const LineRec& L) {  // θ* of an item line, whether or not the line has been processed
  Item& m = S.it[L.item];
  uint64_t t = m.th.load(std::memory_order_relaxed);
  if (!t) { t = L.ts + group_rank(S, ss, L); m.th.store(t, std::memory_order_relaxed); }
  return t;
}
// rs of an injected item = θ* of the first line of its bundle (the maximal run of K_DEV item lines it is in)
uint64_t dev_rs(Store& S, const Sess& ss, const LineRec& L) {
  const LineRec* F = &L;
  for (;;) {
    const LineRec* P = line_ending(S, ss.sg, F->off);
    if (!P || !(P->fl & LF_ITEM) || S.it[P->item].kind != K_DEV) break;
    if (const uint64_t k = S.it[P->item].rs.load(std::memory_order_relaxed)) return k;  // an earlier member knows it: all members share it
    F = P;
  }
  return item_theta(S, ss, *F);
}
// The bundle of K_DEV item lines right before the line X is closed by X (X is not a K_DEV item): its members are put into the instruction state, which is
// the max-merge of the closed bundles (copy, put, replace by CAS: the state is a function of the set of the bundles, whatever the order of the puts).
void close_bundle(Store& S, Sess& ss, const LineRec& X) {
  std::vector<const LineRec*> mem;
  for (uint64_t cur = X.off;;) {
    const LineRec* P = line_ending(S, ss.sg, cur);
    if (!P || !(P->fl & LF_ITEM) || S.it[P->item].kind != K_DEV) break;
    mem.push_back(P); cur = P->off;
  }
  if (mem.empty()) return;
  std::reverse(mem.begin(), mem.end());
  for (const LineRec* M : mem) { item_theta(S, ss, *M); Item& m = S.it[M->item]; if (!m.rs.load(std::memory_order_relaxed)) m.rs.store(dev_rs(S, ss, *M), std::memory_order_relaxed); }
  EbrGuard guard(S);
  for (;;) {
    const InstrState* cur = ss.ist.load(std::memory_order_acquire);
    auto* nw = cur ? new InstrState(*cur) : new InstrState;
    for (const LineRec* M : mem) nw->put(S.it, M->item);
    if (ss.ist.compare_exchange_strong(cur, nw, std::memory_order_acq_rel)) { if (cur) retire_obj(S, const_cast<InstrState*>(cur), 3, sizeof(InstrState)); return; }
    delete nw;
  }
}
void free_td_if_closed(Store& S, TurnSlot& sl, uint32_t lid) {  // a line pushed into a slot that has closed: nobody will read its data
  if (!sl.closed.load(std::memory_order_seq_cst)) return;
  if (TD* d = S.lr[lid].td.exchange(nullptr, std::memory_order_acq_rel)) retire_obj(S, d, 4, sizeof(TD) + d->nt.size() + d->text.size());
}
// One line that is visible (every line before it is announced). It reads the ordinal tree for the lines before it (they were added when those lines were announced),
// writes its cells by position, adds itself to the turn it belongs to, and may close that turn or the one before. Its record is written to the shard in `wb`.
void process_line(Store& S, Sess& ss, uint32_t lid, std::string& wb) {
  LineRec& L = S.lr[lid];
  const uint64_t off = L.off;
  bool dec = false;
  const uint32_t r = group_rank(S, ss, L, &dec);
  L.r = r; L.th = L.ts + r;
  S.dbg_checked.fetch_add(1, std::memory_order_relaxed);
  if (dec) S.dbg_dec.fetch_add(1, std::memory_order_relaxed);
  if (dec || r + 1 > 1048) S.dbg_mismatch.fetch_add(1, std::memory_order_relaxed);  // P1 broken here, or the group is wider than a ms (θ* of two lines could meet)
  { uint64_t mg = S.dbg_maxgroup.load(std::memory_order_relaxed); while (r + 1 > mg && !S.dbg_maxgroup.compare_exchange_weak(mg, r + 1, std::memory_order_relaxed)) {} }
  const bool item = (L.fl & LF_ITEM) != 0, user = (L.fl & LF_USER) != 0, start = (L.fl & LF_START) != 0;
  const bool have = !ss.have_rec.empty() && ss.have_rec.count(off) != 0;
  uint32_t ib = 0, ub = 0, sb = 0;
  if (L.ord != UINT32_MAX) {
    uint64_t a, b; ss.ot.prefix(L.ord, a, b);
    ib = uint32_t(a); ub = uint32_t(a >> 32); sb = uint32_t(b);
    for (uint64_t cur = off;;) {  // lines with the same ordinal right before this one (the rollout repeats an ordinal on a few event lines)
      const LineRec* P = line_ending(S, ss.sg, cur);
      if (!P || P->ord != L.ord) break;
      ib += (P->fl & LF_ITEM) ? 1u : 0u; ub += (P->fl & LF_USER) ? 1u : 0u; sb += (P->fl & LF_START) ? 1u : 0u; cur = P->off;
    }
  } else {  // a line without a usable ordinal counts nothing: the counts are those of the line before it
    const LineRec* P = line_ending(S, ss.sg, off);
    if (P && P->done.load(std::memory_order_acquire)) { ib = P->ia; ub = P->ua; sb = P->sa; } else if (!P) { ib = ss.base.ia; ub = ss.base.ua; sb = ss.base.sa; }
  }
  L.ia = ib + (item ? 1u : 0u); L.ua = ub + (user ? 1u : 0u); L.sa = sb + (start ? 1u : 0u);
  if (item) {
    Item& m = S.it[L.item];
    m.th.store(L.th, std::memory_order_relaxed); m.pos = ib;
    if (m.kind == K_DEV) m.rs.store(dev_rs(S, ss, L), std::memory_order_relaxed);
    if (m.cid) link_call(S, ss.sg, L.item);  // after th is set: readers filter the group by θ
    ss.sess.set(ib, L.item);
    if (user) ss.usr.set(ub, L.item);
    if (!have) emit_item(wb, m, L.ts, L.r, L.ord, L.pfx);
  }
  if (!(item && S.it[L.item].kind == K_DEV)) close_bundle(S, ss, L);  // any other line closes the bundle before it
  if ((L.fl & LF_META) && !have) emit_meta(wb, off, L.len, L.ord, L.ts, L.r, ss);
  if (start && !have) emit_ts(wb, off, L.len, L.ord, L.ord, L.ts, L.r);
  if (L.ord == UINT32_MAX) return;
  const uint32_t m = L.sa;  // the turn slot of the line: the number of task_started lines up to and including it
  if (start) {
    if (m > ss.rturns) {
      TurnSlot& sl = ss.slots.at(m);
      if (sl.start_ready.load(std::memory_order_seq_cst)) S.dup_starts.fetch_add(1, std::memory_order_relaxed);  // two starts of one rank: not supported
      sl.o0.store(off, std::memory_order_release); sl.th0.store(L.th, std::memory_order_release); sl.ord0.store(L.ord, std::memory_order_release); sl.pos0.store(ib, std::memory_order_release);
      sl.start_ready.store(1, std::memory_order_seq_cst);
    }
    if (m >= 2 && m - 1 > ss.rturns) { lower_E(ss.slots.at(m - 1), (off << 1) | 1); try_close(S, ss, m - 1); }  // E2-52: a turn cut without task_complete ends where the next starts
  } else if ((L.fl & LF_COMPLETE) && m >= 1 && m > ss.rturns) lower_E(ss.slots.at(m), off << 1);  // lowered before the line is pushed
  if (m >= 1 && m > ss.rturns) {
    TurnSlot& sl = ss.slots.at(m);
    push_line(S, sl, lid);
    try_close(S, ss, m);
    free_td_if_closed(S, sl, lid);
  }
}
void advance_pub(Store& S, Sess& ss) {  // the processed prefix: over the lines whose done flag is set (and NUL-only holes)
  for (;;) {
    uint64_t p = ss.pub.load(std::memory_order_seq_cst), q = p;
    const LineRec* last = nullptr;
    for (;;) {
      const LineRec* L = line_at(S, ss.sg, q);
      if (L && L->done.load(std::memory_order_seq_cst)) { q = L->off + L->len + 1; last = L; continue; }
      if (const uint64_t h = hole_after(S, ss.sg, q)) { q = h; continue; }
      break;
    }
    if (q == p) return;
    if (!ss.pub.compare_exchange_strong(p, q, std::memory_order_seq_cst)) continue;
    if (last && last->ord != UINT32_MAX) {
      uint64_t o = ss.pubord.load(std::memory_order_relaxed);
      while (uint64_t(last->ord) + 1 > o && !ss.pubord.compare_exchange_weak(o, uint64_t(last->ord) + 1, std::memory_order_seq_cst)) {}
    }
    mark_wm(S, ss);
    if (ss.waiters.load(std::memory_order_seq_cst)) wake_waiters(S, ss);
    if (S.pub_waiters.load(std::memory_order_seq_cst)) bump_progress(S);  // KW3: a vrs_wait_off caller sleeps on the progress word
  }
}
void process_run(Store& S, Sess& ss, std::vector<uint32_t>& run) {
  EbrGuard guard(S);
  std::string wb;
  for (uint32_t lid : run) process_line(S, ss, lid, wb);
  wflush(S, ss, wb);
  for (uint32_t lid : run) S.lr[lid].done.store(1, std::memory_order_seq_cst);
  S.lines_applied.fetch_add(run.size(), std::memory_order_relaxed);
  S.runs.fetch_add(1, std::memory_order_relaxed);
  { uint64_t mx = S.run_max.load(std::memory_order_relaxed); while (run.size() > mx && !S.run_max.compare_exchange_weak(mx, run.size(), std::memory_order_relaxed)) {} }
  if (S.pend_total.fetch_sub((int64_t)run.size()) <= (int64_t)run.size()) maybe_ready(S);
  advance_pub(S, ss);
}
// The lines announced right behind `front` are taken over by one CAS on front; a thread that loses the CAS finds that somebody has them. The announcer publishes the
// line (seq_cst) and then reads front, the taker moves front and then reads the line: one of them sees the other, so no announced line is left behind.
void claim_and_run(Store& S, Sess& ss) {
  for (;;) {
    uint64_t f = ss.front.load(std::memory_order_seq_cst), q = f;
    std::vector<uint32_t> run;
    for (;;) {
      const auto* nd = S.lmap.find(lk(ss.sg, q));
      const uint32_t v = nd ? nd->val.load(std::memory_order_seq_cst) : 0;
      if (v && v != CLAIMING) { run.push_back(v); const LineRec& L = S.lr[v]; q = L.off + L.len + 1; continue; }
      if (const uint64_t h = hole_after(S, ss.sg, q)) { q = h; continue; }
      break;
    }
    if (q == f) return;
    if (!ss.front.compare_exchange_strong(f, q, std::memory_order_seq_cst)) continue;  // somebody took them: look again
    process_run(S, ss, run);
  }
}
// A line is handed in (vrs_insert, a read of the file). Its record is filled and its counts are added to the ordinal tree before it is announced. false = known already.
bool insert_line(Store& S, Sess& ss, uint64_t off, const char* p, size_t len, bool claim) {
  if (off + len + 1 <= ss.base_end || off >= (uint64_t(1) << 40)) return false;
  auto* nd = S.lmap.get(lk(ss.sg, off));
  uint32_t z = 0;
  if (!nd->val.compare_exchange_strong(z, CLAIMING, std::memory_order_acq_rel)) return false;  // announced or being announced by somebody else
  const std::string line(p, len);
  const auto ty = find_field(line, "\"type\":\"", 120);
  bool isitem = line.find("\"type\":\"response_item\"") != std::string::npos;
  const uint32_t lid = S.nline.fetch_add(1) + 1;
  LineRec& L = S.lr.ensure(lid);
  L.off = off; L.len = (uint32_t)len;
  L.ts = parse_ts(sval(line, "timestamp"));
  L.pfx = chk32(line.data(), std::min<size_t>(line.size(), 64));
  { const uint64_t o = numval(line, "ordinal"); if (o == UINT64_MAX || o >= (uint64_t(1) << 24)) { L.ord = UINT32_MAX; L.fl |= LF_BAD; S.bad_lines.fetch_add(1, std::memory_order_relaxed); } else L.ord = (uint32_t)o; }
  if (const auto* he = S.holes_end.find(lk(ss.sg, off))) { const uint64_t a = he->val.load(std::memory_order_acquire); if (a < off) L.gap = (uint32_t)(off - a); }
  if (L.fl & LF_BAD) isitem = false;  // a line without an ordinal has no place in the lists: it counts nothing
  if (off == 0) {  // the first line of a file: session_meta (id, fork)
    if (line.find("\"type\":\"session_meta\"") != std::string::npos) {
      ss.id = sval(line, "id"); ss.fpar = sval(line, "forked_from_id");
      const uint64_t k = numval(line, "forked_from_ordinal_exclusive"); ss.fk = k == UINT64_MAX ? 0 : k;
      L.fl |= LF_META;
      if (!ss.id.empty()) S.idmap.get(fnv(ss.id.data(), ss.id.size()))->val.store(ss.sg, std::memory_order_release);
    }
    ss.meta_pub.store(1, std::memory_order_release);
  }
  if (ty == "event_msg") {
    const auto pt = find_field(line, "\"payload\":{\"type\":\"", 200);
    if (pt == "task_started") L.fl |= LF_START; else if (pt == "task_complete") L.fl |= LF_COMPLETE;
  }
  if (isitem) {
    const uint32_t id = S.nit.fetch_add(1) + 1;
    Item& m = S.it.ensure(id);
    m.sess = ss.sg; m.kind = kind_of(line); m.fwd = m.kind == K_USER && is_fwd(line);
    m.tok = codex_tokens(line);  // L1 v24 D16 / E2-59
    m.off = off; m.len = (uint32_t)len; m.ord = L.ord;
    if (m.kind == K_DEV) { m.ch = content_hash(line); parts_of(line, m); }
    const std::string c = sval(line, "call_id");
    if (!c.empty()) { const uint64_t h = fnv(c.data(), c.size()); m.cid = h ? h : 1; }
    if (ss.memmode) m.mem = line;
    L.item = id; L.fl |= LF_ITEM | (m.kind == K_USER ? LF_USER : 0) | (m.kind == K_DEV ? LF_DEV : 0);
  }
  if (!(L.fl & LF_BAD)) L.td.store(make_td(S, line, L.item), std::memory_order_release);
  if (L.ord != UINT32_MAX) {
    const uint64_t da = (L.fl & LF_ITEM) ? (1ull | ((L.fl & LF_USER) ? (1ull << 32) : 0)) : 0, db = (L.fl & LF_START) ? 1 : 0;
    if (da || db) ss.ot.add(L.ord, da, db);
  }
  S.lend.get(lk(ss.sg, off + len + 1))->val.store(lid, std::memory_order_release);
  S.lines_in.fetch_add(1, std::memory_order_relaxed);
  S.pend_total.fetch_add(1, std::memory_order_seq_cst);
  nd->val.store(lid, std::memory_order_seq_cst);  // announced
  if (claim) claim_and_run(S, ss);
  return true;
}

// ---------------- what a reader sees of a file: the processed prefix ----------------
vrs_visibility& visibility_trace() {
#ifdef _WIN32
  return VP_TLS(vrs_visibility);
#else
  thread_local vrs_visibility trace{};
  return trace;
#endif
}
struct PubView {
  uint64_t P = 0; const LineRec* last = nullptr;
  uint64_t lastth = 0; uint32_t ia = 0, ua = 0, sa = 0, ntl = 0; bool open = false;
};
// The state at the end of the processed prefix. Every line before pub is done, so its record holds the counts of the prefix (items, users, starts), and a turn
// whose end line is in the prefix has been closed by the time the end line was done: the turn list is complete below ntl.
PubView pub_view(const Store& S, const Sess& ss) {
  PubView v; v.P = ss.pub.load(std::memory_order_seq_cst);
  for (uint64_t q = v.P;;) {
    v.last = line_ending(S, ss.sg, q);
    if (v.last) break;
    const auto* he = S.holes_end.find(lk(ss.sg, q));
    if (!he) break;
    q = he->val.load(std::memory_order_acquire);
  }
  if (v.last) { v.lastth = v.last->th; v.ia = v.last->ia; v.ua = v.last->ua; v.sa = v.last->sa; }
  else { v.lastth = ss.base.has ? ss.base.th : 0; v.ia = ss.base.ia; v.ua = ss.base.ua; v.sa = ss.base.sa; }
  const uint32_t m = v.sa;
  if (m == 0) { v.ntl = 0; v.open = false; }
  else if (m <= ss.rturns) { v.ntl = m; v.open = false; }
  else {
    bool vis = false;
    if (const TurnSlot* sl = ss.slots.find(m)) if (sl->closed.load(std::memory_order_seq_cst)) vis = (sl->E.load(std::memory_order_seq_cst) >> 1) < v.P;  // its end line is in the prefix
    v.ntl = vis ? m : m - 1; v.open = !vis;
  }
  return v;
}
// L1 v18 D6: the state of a request on a file. false = the lineage cannot be told yet (VI-11: the parent has not reached the fork ordinal), the caller says rc 1.
bool read_state(Store& S, Sess& ss, Snap& sn) {
  sn.segok = ss.seg_state.load(std::memory_order_seq_cst) == 2;
  if (!sn.segok && ss.meta_pub.load(std::memory_order_acquire) && !ss.fpar.empty()) { sn.lin_pending = true; return false; }
  const uint64_t required = ss.insert_end.load(std::memory_order_seq_cst);
  const PubView v = pub_view(S, ss);
  auto& trace = visibility_trace();
  trace.view_sg = ss.sg; trace.pub = v.P; trace.front = ss.front.load(std::memory_order_seq_cst);
  if (v.P < required) return false; // N-F: this exact view must cover the explicitly supplied input
  sn.lastth = v.lastth; sn.nsess = v.ia; sn.nusr = v.ua; sn.ntl = v.ntl;
  if (v.open) {  // the open turn: the lines of its slot below the end of the prefix
    const TurnSlot* slp = ss.slots.find(v.sa);
    if (!slp) { sn.open = false; }  // no slot yet: the start line of the turn has not been processed; there is no open turn to read
    else {
    const TurnSlot& sl = *slp;
    TurnAgg g; g.o0 = sl.o0.load(); g.th0 = sl.th0.load(); g.ord0 = sl.ord0.load(); g.pos0 = sl.pos0.load(); g.ordl = g.ord0;
    for (uint32_t id = sl.lst.load(std::memory_order_seq_cst); id; id = S.lr[id].nextl) {
      const LineRec& L = S.lr[id];
      if (L.off >= g.o0 && L.off < v.P) agg_add(S, g, L, L.td.load(std::memory_order_acquire));
    }
    TurnParse p; agg_parse(g, p);
    sn.open = true; sn.pos0 = g.pos0; sn.th0 = g.th0; sn.tq1 = p.tq1; sn.q1 = std::move(p.q1); sn.ocalls = agg_ocalls_set(g);
    }
  }
  // I(t, Λ): the closed bundles, and the bundle that is still open at the end of the prefix (the K_DEV item lines right before it)
  const InstrState* base = ss.ist.load(std::memory_order_acquire);
  std::vector<uint32_t> run;
  for (const LineRec* P = v.last; P && (P->fl & LF_ITEM) && S.it[P->item].kind == K_DEV; P = line_ending(S, ss.sg, P->off)) run.push_back(P->item);
  if (run.empty() && base) sn.ist = base;
  else {
    sn.own_ist.reset(base ? new InstrState(*base) : new InstrState);
    for (size_t i = run.size(); i > 0; i--) sn.own_ist->put(S.it, run[i - 1]);
    sn.ist = sn.own_ist.get();
  }
  return true;
}

// ---------------- lineage: forks (L2 7판, D4) ----------------
// The inherited prefix of a fork is computed from the parents' lists and the cut `forked_from_ordinal_exclusive`; it needs no ingest of the parent.
uint32_t find_parent(const Store& S, const std::string& pid) {
  if (pid.empty()) return 0;
  if (const auto* nd = S.idmap.find(fnv(pid.data(), pid.size()))) { const uint32_t v = nd->val.load(std::memory_order_acquire); if (v) return v; }
  // VI-09: the id in the file name (filled when a file is found: restore, watcher, first insert). A miss = the parent file is not in the store.
  if (const auto* nd = S.fidmap.find(fnv(pid.data(), pid.size()))) { const uint32_t v = nd->val.load(std::memory_order_acquire); if (v) return v; }
  return 0;
}
struct LinWait { bool pending = false; uint32_t blk = 0; uint64_t need = 0; };
// VI-11: the lineage is a function of the set. It can be computed when the parent (and its parents) have processed every line before the fork ordinal; until then
// `w` names the session that is short and the ordinal it must reach. force = take what is there (the first look at a restored file, where the file set is
// what a fresh 42f would read).
std::vector<std::array<uint32_t, 4>> compute_segs(const Store& S, uint32_t sg, int depth, bool force, LinWait& w) {
  const Sess& ss = S.sx(sg);
  if (!ss.meta_pub.load(std::memory_order_acquire) || ss.fpar.empty() || depth > 16) return {};
  const uint32_t psg = find_parent(S, ss.fpar);
  if (!psg || psg == sg) return {};
  const uint64_t k = ss.fk;
  const Sess& ps = S.sx(psg);
  if (!force && ps.pubord.load(std::memory_order_seq_cst) < k) { w.pending = true; w.blk = psg; w.need = k; return {}; }
  auto P = compute_segs(S, psg, depth + 1, force, w);
  if (w.pending) return {};
  const PubView pv = pub_view(S, ps);
  auto cut = [&](const Cells& v, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) { const size_t md = (lo + hi) / 2; if (S.it[v[md]].ord < k) lo = md + 1; else hi = md; }
    return (uint32_t)lo; };
  auto cutt = [&](const Cells& v, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) { const size_t md = (lo + hi) / 2; if (S.TT[v[md]]->ord1 < k) lo = md + 1; else hi = md; }
    return (uint32_t)lo; };
  std::vector<std::array<uint32_t, 4>> out;
  for (auto& g : P) out.push_back({g[0], cut(S.sess[g[0]], g[1]), cut(S.usr[g[0]], g[2]), cutt(S.tl[g[0]], g[3])});
  out.push_back({psg, cut(ps.sess, pv.ia), cut(ps.usr, pv.ua), cutt(ps.tl, pv.ntl)});
  return out;
}
void setup_waiter(Store& S, Sess& ss, const LinWait& w) {  // register ss with the session it waits for (once per target), then look again
  const uint64_t key = (uint64_t(w.blk) << 40) ^ (w.need + 1);
  if (ss.wait_key.exchange(key, std::memory_order_seq_cst) == key) return;
  Sess& b = S.sx(w.blk);
  WNode* n = new WNode{ss.sg, w.need, nullptr};
  WNode* h = b.waiters.load(std::memory_order_relaxed);
  do { n->next = h; } while (!b.waiters.compare_exchange_weak(h, n, std::memory_order_seq_cst, std::memory_order_relaxed));
  S.lin_waits.fetch_add(1, std::memory_order_relaxed);
  if (b.pubord.load(std::memory_order_seq_cst) >= w.need) {  // it got there while we registered
    ss.wait_key.store(0, std::memory_order_seq_cst);
    if (!S.work.push(task_of(T_KICK, ss.sg))) ensure_segs(S, ss, false);
  }
}
// a session has processed lines: tell the children that were waiting for an ordinal it has now reached
void wake_waiters(Store& S, Sess& ss) {
  WNode* h = ss.waiters.exchange(nullptr, std::memory_order_seq_cst);
  while (h) {
    WNode* nx = h->next;
    if (ss.pubord.load(std::memory_order_seq_cst) >= h->need) {
      Sess& c = S.sx(h->child);
      c.wait_key.store(0, std::memory_order_seq_cst);
      if (!S.work.push(task_of(T_KICK, h->child))) ensure_segs(S, c, false);
      delete h;
    } else {
      WNode* t = ss.waiters.load(std::memory_order_relaxed);
      do { h->next = t; } while (!ss.waiters.compare_exchange_weak(t, h, std::memory_order_seq_cst, std::memory_order_relaxed));
    }
    h = nx;
  }
}
// L2 v16: the instruction state of a lineage = the puts of the inherited injected items up to the cut and of the flow's own kept items. The state is a max-merge, so
// the inherited puts are merged into whatever the file has put by itself already.
void merge_inherited(Store& S, Sess& ss, const std::vector<std::array<uint32_t, 4>>& segs) {
  std::vector<uint32_t> inh;
  for (auto& g : segs) for (uint32_t q = 0; q < g[1]; q++) { const uint32_t m = S.sess[g[0]][q]; if (S.it[m].kind == K_DEV) inh.push_back(m); }
  if (inh.empty()) return;
  EbrGuard guard(S);
  for (;;) {
    const InstrState* cur = ss.ist.load(std::memory_order_acquire);
    auto* nw = cur ? new InstrState(*cur) : new InstrState;
    for (uint32_t m : inh) nw->put(S.it, m);
    if (ss.ist.compare_exchange_strong(cur, nw, std::memory_order_acq_rel)) { if (cur) retire_obj(S, const_cast<InstrState*>(cur), 3, sizeof(InstrState)); return; }
    delete nw;
  }
}
// set the lineage of ss up (once). true = it is set up. Nobody waits: a thread that finds it being set up by another leaves it to that thread, which goes on to count.
bool ensure_segs(Store& S, Sess& ss, bool force) {
  if (ss.seg_state.load(std::memory_order_seq_cst) == 2) return true;
  if (!ss.meta_pub.load(std::memory_order_acquire)) return false;  // no first line yet
  LinWait w;
  auto segs = compute_segs(S, ss.sg, 0, force, w);
  if (w.pending) { ss.lin_pending.store(1, std::memory_order_release); setup_waiter(S, ss, w); return false; }
  int z = 0;
  if (!ss.seg_state.compare_exchange_strong(z, 1, std::memory_order_seq_cst)) return ss.seg_state.load(std::memory_order_seq_cst) == 2;
  if (!segs.empty()) merge_inherited(S, ss, segs);  // before the segments are published: whoever sees them sees the merged state
  ss.segs.store(new Segs{std::move(segs)}, std::memory_order_release);
  ss.lin_pending.store(0, std::memory_order_release);
  ss.seg_state.store(2, std::memory_order_seq_cst);
  bump_progress(S);  // P-14
  drive_counts(S, ss);
  return true;
}

// ---------------- D11 / P-10: contribution units, replaced when the windows they were counted over change ----------------
// Y(x): the turns whose observers a unit of x can depend on: the candidates, the far ones, and every turn of BC(x₋d) (A0 is a subset of it). It is a function of the
// lineage, not of any observer.
std::vector<uint32_t> ystatic(Store& S, uint32_t x, const View& TV, size_t ix) {
  const TurnRec& X = *S.TT[x];
  std::vector<uint32_t> Y;
  for (auto& b : base_pairs(S, x, TV, ix)) Y.push_back(b.y);
  for (size_t j = ix > FAR_MAX - 1 ? ix - (FAR_MAX - 1) : 0; j + KD < ix; j++) { const uint32_t y = TV[j]; if (S.TT[y]->the < X.the) Y.push_back(y); }
  for (int d = 1; d <= KV && (size_t)d <= ix; d++) {
    const uint32_t xp = TV[ix - d];
    if (S.TT[xp]->the > X.th0) continue;
    for (auto& b : base_pairs(S, xp, TV, ix - d)) Y.push_back(b.y);
  }
  std::sort(Y.begin(), Y.end()); Y.erase(std::unique(Y.begin(), Y.end()), Y.end());
  return Y;
}
void register_deps(Store& S, uint32_t x, const std::vector<uint32_t>& Y) {
  if (S.TT[x]->regd.exchange(1, std::memory_order_seq_cst)) return;
  for (uint32_t y : Y) deps_add(S, y, x);
}
// the unit of x over the 𝔅 of one pass: Φ = the sum of the window fingerprints of Y, and the cells of every pair (a pass that saw 𝔅 move is done again)
Contrib* build_unit(Store& S, uint32_t x, const View& TV, size_t ix, uint32_t root, const std::vector<uint32_t>& Y) {
  for (;;) {
    PassScope ps(S);
    std::vector<uint64_t> ph(Y.size()); uint64_t phi = 0;
    for (size_t i = 0; i < Y.size(); i++) { ph[i] = win_in_pass(S, Y[i], &ps.p).phi; phi += ph[i]; }
    std::vector<std::array<uint32_t, 3>> rec;
    count_pairs(S, x, TV, ix, root, nullptr, nullptr, &rec);
    if (!ps.ok()) { if (S.pass_retries.fetch_add(1, std::memory_order_relaxed) % 8 == 7) std::this_thread::yield(); continue; }
    Contrib* c = make_contrib(rec, root, phi);
    c->Y = Y; c->ph = std::move(ph); c->ny = Y.size();
    return c;
  }
}
bool unit_valid(Store& S, const Contrib* c) {  // is the unit the unit of the windows of Y now?
  PassScope ps(S);
  uint64_t phi = 0;
  for (uint32_t y : c->Y) phi += win_in_pass(S, y, &ps.p).phi;
  // A unit is the unit of the windows only when a pass that saw no turn come in (or go out of the middle of coming in) found the fingerprint it was made over.
  // While 𝔅 is moving nothing is concluded: the caller counts again (build_unit repeats its pass until it saw one state). The turn that moves it looks at the units that
  // depend on it when it is in, but a turn that finished before that look and met a unit not installed yet (null) skipped it; this check after the install is what catches that.
  return ps.p.stable() && phi == c->phi;
}
// Count x until the unit installed is the unit of the windows now (D11): build, install by CAS, apply the difference to the cells, and look again; a unit that was
// made over windows another turn has changed since is replaced. The initial count and every later one are this function.
void converge(Store& S, Sess& ss, const View& TV, size_t ix, uint32_t x, uint32_t root) {
  EbrGuard guard(S);
  TurnRec& t = *S.TT[x];
  const Contrib* c0 = t.ctr.load(std::memory_order_acquire);
  const std::vector<uint32_t> Y = c0 ? c0->Y : ystatic(S, x, TV, ix);  // Y is a function of the lineage: a unit already made has it
  register_deps(S, x, Y);
  for (;;) {
    Contrib* c = t.ctr.load(std::memory_order_acquire);
    if (c && unit_valid(S, c)) return;
    Contrib* nw = build_unit(S, x, TV, ix, root, Y);
    if (t.ctr.compare_exchange_strong(c, nw, std::memory_order_acq_rel)) {
      apply_unit(S, nw, c);
      S.ctr_bytes.fetch_add(sizeof(Contrib) + nw->e.size() * sizeof(CEnt) + nw->Y.size() * 12, std::memory_order_relaxed);
      if (c) {
        S.ctr_bytes.fetch_sub(sizeof(Contrib) + c->e.size() * sizeof(CEnt) + c->Y.size() * 12, std::memory_order_relaxed);
        retire_obj(S, c, 2, sizeof(Contrib) + c->e.size() * sizeof(CEnt) + c->Y.size() * 12);
        S.recounts.fetch_add(1, std::memory_order_relaxed);
      } else {
        S.ctr_n.fetch_add(1, std::memory_order_relaxed);
        if (S.pend_total.fetch_sub(1, std::memory_order_seq_cst) <= 1) maybe_ready(S);
      }
      if (!ss.memmode) { std::string b; emit_contrib2(b, t.o0, *nw); shard_write(S, ss, b); }
      continue;  // look again: a turn may have come in between the pass and the install
    }
    delete nw;
  }
}
// restored units: the shard holds the unit a past run counted x over, with the fingerprint of its set. It is the unit of x now when the fingerprint is the one of
// the windows now; otherwise x is counted again.
void settle_one(Store& S, Sess& ss, const View& TV, size_t ix, uint32_t x, uint32_t root) {
  TurnRec& t = *S.TT[x];
  Contrib* cands = t.cand; t.cand = nullptr;
  if (!cands) { converge(S, ss, TV, ix, x, root); return; }
  EbrGuard guard(S);
  const std::vector<uint32_t> Y = ystatic(S, x, TV, ix);
  register_deps(S, x, Y);
  Contrib* pick = nullptr;
  {
    PassScope ps(S);
    std::vector<uint64_t> ph(Y.size()); uint64_t phi = 0;
    for (size_t i = 0; i < Y.size(); i++) { ph[i] = win_in_pass(S, Y[i], &ps.p).phi; phi += ph[i]; }
    for (Contrib* q = cands; q; q = q->next) if (!pick && q->ny == Y.size() && q->phi == phi) { pick = q; pick->ph = ph; }
  }
  for (Contrib* q = cands; q;) { Contrib* nx = q->next; if (q != pick) delete q; q = nx; }
  if (pick) {
    pick->next = nullptr; pick->Y = Y; pick->root = root;
    Contrib* e = nullptr;
    if (t.ctr.compare_exchange_strong(e, pick, std::memory_order_acq_rel)) {
      apply_unit(S, pick, nullptr);
      S.ctr_n.fetch_add(1, std::memory_order_relaxed); S.settle_ok.fetch_add(1, std::memory_order_relaxed);
      S.ctr_bytes.fetch_add(sizeof(Contrib) + pick->e.size() * sizeof(CEnt) + pick->Y.size() * 12, std::memory_order_relaxed);
      if (S.pend_total.fetch_sub(1, std::memory_order_seq_cst) <= 1) maybe_ready(S);
    } else delete pick;
  } else S.settle_bad.fetch_add(1, std::memory_order_relaxed);
  converge(S, ss, TV, ix, x, root);  // the unit is checked again against the windows now (a turn may have come in), or x is counted
}
void count_rank(Store& S, Sess& ss, uint32_t r) {
  const uint32_t x = ss.tl[r];
  const Segs* sg = ss.segs.load(std::memory_order_acquire);
  View TV;
  if (sg) for (auto& g : sg->v) TV.add(g[0], &S.tl[g[0]], g[3]);
  TV.add(ss.sg, &ss.tl, (size_t)r + 1);
  const size_t ix = TV.idx(ss.sg, r);
  const uint32_t root = (sg && !sg->v.empty()) ? S.sx(sg->v[0][0]).root32 : ss.root32;
  settle_one(S, ss, TV, ix, x, root);
}
struct CountBatch { uint32_t sg, a, e; };
void run_count(Store& S, CountBatch* b) {
  Sess& ss = S.sx(b->sg);
  for (uint32_t r = b->a; r < b->e; r++) count_rank(S, ss, r);
  delete b;
}
// the turns of ss whose record is in the list (a written prefix) and whose lineage is set up are counted: the front of the counting moves by CAS over them
void drive_counts(Store& S, Sess& ss) {
  if (!S.loaded.load(std::memory_order_seq_cst) || ss.seg_state.load(std::memory_order_seq_cst) != 2) return;
  for (;;) {
    uint32_t f = ss.cfront.load(std::memory_order_seq_cst);
    const uint32_t n = (uint32_t)ss.tl.size();
    if (f >= n) return;
    if (!ss.cfront.compare_exchange_strong(f, n, std::memory_order_seq_cst)) continue;
    for (uint32_t a = f; a < n; a += 8) {
      auto* b = new CountBatch{ss.sg, a, std::min(n, a + 8)};
      if (!S.work.push(task_of(T_COUNT, (uint64_t)(uintptr_t)b))) run_count(S, b);
    }
  }
}
// P-10: a turn z is in 𝔅. The turns whose window can have changed are Y_z (T-int-8: at most W - 1 + T of them), found from the position of z upward; the units that
// hold one of them are looked at again, in batches, by whoever takes them from the work set. No global counter, no waiting.
struct RevBatch { std::vector<std::pair<uint32_t, uint32_t>> xy; };
void run_revisit(Store& S, RevBatch* b) {
  EbrGuard guard(S);
  for (auto& [x, y] : b->xy) {
    S.reval_checked.fetch_add(1, std::memory_order_relaxed);
    TurnRec& t = *S.TT[x];
    Contrib* c = t.ctr.load(std::memory_order_acquire);
    if (!c) continue;  // not counted yet: its count is over the windows then
    const auto it = std::lower_bound(c->Y.begin(), c->Y.end(), y);
    if (it == c->Y.end() || *it != y) continue;
    if (c->ph[it - c->Y.begin()] == window_of(S, *S.TT[y]).phi) continue;  // the window of y is the one the unit was made over
    Sess& ss = S.sx(t.sg);
    const Segs* sg = ss.segs.load(std::memory_order_acquire);
    if (!sg) continue;
    View TV;
    for (auto& g : sg->v) TV.add(g[0], &S.tl[g[0]], g[3]);
    TV.add(ss.sg, &ss.tl, (size_t)t.tpos + 1);
    converge(S, ss, TV, TV.idx(ss.sg, t.tpos), x, c->root);
  }
  delete b;
  if (S.pend_total.fetch_sub(1, std::memory_order_seq_cst) <= 1) maybe_ready(S);
}
void on_turn_in(Store& S, uint32_t z) {
  if (!S.loaded.load(std::memory_order_seq_cst)) return;  // before the load nothing is counted; the first counts see this turn
  const TurnRec& tz = *S.TT[z];
  const uint64_t p0 = S.B.rank(tz.the + 1);  // the turns with theta_e <= theta_e(z), z among them
  std::vector<uint32_t> Yz;
  for (uint64_t p = p0, tot = S.B.total.load(std::memory_order_acquire); p < tot; p++) {
    const uint32_t y = S.B.select(p);
    if (!y) break;
    if (!(S.TT[y]->the > tz.the)) continue;  // not above z (a turn that came in meanwhile moved the positions)
    if ((int64_t)S.B.rank(S.TT[y]->the) - (int64_t)S.B.rank(tz.the + 1) >= (int64_t)OBS_W) break;  // z is below the window of y and of every turn above (z's rank is read again: a turn that came in below moves both)
    Yz.push_back(y);
  }
  if (Yz.empty()) return;
  S.late_events.fetch_add(1, std::memory_order_relaxed);
  { uint64_t m = S.y_max.load(std::memory_order_relaxed); while (Yz.size() > m && !S.y_max.compare_exchange_weak(m, Yz.size(), std::memory_order_relaxed)) {} }
  S.y_total.fetch_add(Yz.size(), std::memory_order_relaxed);
  uint64_t pairs = 0;
  auto* cur = new RevBatch;
  auto flush = [&](bool last) {
    if (cur->xy.size() < 64 && !last) return;
    if (cur->xy.empty()) { delete cur; cur = nullptr; return; }
    S.pend_total.fetch_add(1, std::memory_order_seq_cst);
    S.revisit_tasks.fetch_add(1, std::memory_order_relaxed);
    if (!S.work.push(task_of(T_REVISIT, (uint64_t)(uintptr_t)cur))) run_revisit(S, cur);
    cur = last ? nullptr : new RevBatch;
  };
  for (uint32_t y : Yz)
    for (const DepChunk* c = S.TT[y]->deps.load(std::memory_order_seq_cst); c; c = c->next.load(std::memory_order_acquire)) {
      const uint32_t n = std::min<uint32_t>(14, c->n.load(std::memory_order_seq_cst));
      for (uint32_t i = 0; i < n; i++) if (const uint32_t x = c->id[i].load(std::memory_order_seq_cst)) { cur->xy.push_back({x, y}); pairs++; if (cur->xy.size() >= 64) flush(false); }
    }
  if (cur) flush(true);
  S.x_total.fetch_add(pairs, std::memory_order_relaxed);
  { uint64_t m = S.x_max.load(std::memory_order_relaxed); while (pairs > m && !S.x_max.compare_exchange_weak(m, pairs, std::memory_order_relaxed)) {} }
}

// ready = every shard is in (loaded), every base session has had its first look after that, and no turn is left uncounted and no line is unprocessed
void maybe_ready(Store& S) {
  if (S.loaded.load(std::memory_order_seq_cst) && S.fin_left.load(std::memory_order_seq_cst) <= 0 && S.pend_total.load(std::memory_order_seq_cst) <= 0) {
    int64_t m = -1;
    S.ready_us.compare_exchange_strong(m, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - S.t_open).count());
#ifdef VRS_DBG_HOOKS
    if (S.dbg_fail_ready) { S.ready.store(-1, std::memory_order_seq_cst); bump_progress(S); return; }  // T6-2: the store failing
    if (S.hold_ready.time_since_epoch().count() && std::chrono::steady_clock::now() < S.hold_ready) {  // T6-2 race injection: latch later
      int a = 0;
      if (S.hold_armed.compare_exchange_strong(a, 1)) std::thread([&S] { std::this_thread::sleep_until(S.hold_ready); maybe_ready(S); }).detach();
      return;
    }
#endif
    int z = 0;
    if (S.ready.compare_exchange_strong(z, 1, std::memory_order_seq_cst)) bump_progress(S);
    k5_maybe_ready(S);
  }
}
// ---------------- restore (P-2) ----------------
bool load_shard(Store& S, Sess& ss, Shard& sh) {
  const int fd = vp_open_ro(shard_path(S, ss).c_str());
  if (fd < 0) return false;
  uint64_t st_size = 0;
  if (!vp_fsize(fd, st_size) || st_size == 0) { vp_close(fd); sh.present = st_size == 0 ? false : sh.present; return false; }
  void* m = vp_map_ro(fd, (size_t)st_size);
  if (m == nullptr) {
    uint32_t magic = 0;
    const ssize_t n = vp_pread(fd, &magic, sizeof magic, 0);
    sh.current = n < (ssize_t)sizeof magic || magic == VX_MAGIC;
    vp_close(fd);
    return false;
  }
  vp_close(fd);
  uint32_t magic = 0;
  if (st_size >= sizeof magic) std::memcpy(&magic, m, sizeof magic);
  sh.current = st_size < sizeof magic || magic == VX_MAGIC;
  if (sh.current) {
    sh.present = true;
    parse_shard((const uint8_t*)m, (size_t)st_size, sh);
  }
  vp_unmap(m, (size_t)st_size);
  return true;
}
bool line_ok(int fd, const ItemP& x) {  // the file still holds this line (prefix hash, newline after it)
  char b[4096]; const size_t want = std::min<size_t>(x.len, 64);
  if (x.len < sizeof b) {
    const size_t n = size_t(x.len) + 1;
    return vp_pread(fd, b, n, x.off) == (ssize_t)n && chk32(b, want) == x.pfx && b[x.len] == '\n';
  }
  if (vp_pread(fd, b, want, x.off) != (ssize_t)want || chk32(b, want) != x.pfx) return false;
  char nl;
  return vp_pread(fd, &nl, 1, x.off + x.len) == 1 && nl == '\n';
}
// P-2 / gap check: the item records must follow each other (pos = 0, 1, 2, ...; byte offsets and line numbers increasing) and agree with
// the file. Anything else sends the file back to a full parse.
bool validate_shard(Store& S, Sess& ss, Shard& sh) {
  auto& I = sh.items;
  std::sort(I.begin(), I.end(), [](const ItemP& a, const ItemP& b) { return a.off < b.off; });
  I.erase(std::unique(I.begin(), I.end(), [](const ItemP& a, const ItemP& b) { return a.off == b.off; }), I.end());
  std::sort(sh.tss.begin(), sh.tss.end(), [](const TsP& a, const TsP& b) { return a.off < b.off; });
  sh.tss.erase(std::unique(sh.tss.begin(), sh.tss.end(), [](const TsP& a, const TsP& b) { return a.off == b.off; }), sh.tss.end());
  std::sort(sh.turns.begin(), sh.turns.end(), [](const TurnP& a, const TurnP& b) { return a.o0 < b.o0; });
  sh.turns.erase(std::unique(sh.turns.begin(), sh.turns.end(), [](const TurnP& a, const TurnP& b) { return a.o0 == b.o0; }), sh.turns.end());
  const auto source = source_fd(ss);
  if (!source) return false;
  const int fd = source->fd;
  uint64_t st_size = 0;
  if (!vp_fsize(fd, st_size)) return false;
  uint64_t maxend = 0;
  bool ok = true;
  for (size_t i = 0; i < I.size() && ok; i++) {
    const ItemP& x = I[i];
    if (x.pos != i) ok = false;
    if (i && !(x.ord >= I[i - 1].ord && x.off >= I[i - 1].off + I[i - 1].len + 1)) ok = false;
    maxend = std::max<uint64_t>(maxend, x.off + x.len + 1);
  }
  for (const auto& t : sh.tss) maxend = std::max<uint64_t>(maxend, t.off + t.len + 1);
  for (const auto& m : sh.metas) maxend = std::max<uint64_t>(maxend, m.off + m.len + 1);
  if (ok && st_size < maxend) ok = false;
  if (ok && !I.empty()) ok = line_ok(fd, I.front()) && line_ok(fd, I.back()) && line_ok(fd, I[I.size() / 2]);
  if (!ok) return false;
  for (auto& t : sh.turns) {  // the items of the turn's view must be in the shard
    t.viidx.clear();
    t.viidx.reserve(t.vi.size());
    for (uint64_t o : t.vi) {
      auto it = std::lower_bound(I.begin(), I.end(), o, [](const ItemP& a, uint64_t v) { return a.off < v; });
      if (it == I.end() || it->off != o) return false;
      t.viidx.push_back((uint32_t)(it - I.begin()));
    }
    if (t.pos0 > t.pos1 || t.pos1 > I.size()) return false;
  }
  return true;
}

// P-3': a shard that holds a record at or beyond its own confirmed mark describes lines the file may not hold (the process died between the record and the
// write): it is not used; the file is parsed again (the NUL-only holes are skipped)
bool shard_ahead(const Shard& sh) {
  if (!sh.has_wm) return false;
  for (const auto& x : sh.items) if (x.off >= sh.wm) return true;
  for (const auto& x : sh.tss) if (x.off >= sh.wm) return true;
  for (const auto& x : sh.metas) if (x.off >= sh.wm) return true;
  for (const auto& x : sh.turns) if (x.o1 >= sh.wm) return true;
  return false;
}


// turn the decoded records into table entries (P-11): everything before `start` is put in the lists, the ordinal tree and the turn list as the lines would have; the
// lines from `start` on are read from the file again. start is the end of the last known line, or the task_started line of the earliest turn that was never
// recorded (the open turn: its aggregate is rebuilt from its lines).
void materialize(Store& S, Sess& ss, Shard& sh) {
  auto& I = sh.items;
  const size_t n = I.size();
  const MetaP* meta = nullptr;
  for (const auto& m : sh.metas) if (m.off == 0 && (!meta || m.off < meta->off)) meta = &m;
  if (meta) {
    ss.id = meta->id; ss.fpar = meta->fpar; ss.fk = meta->fk;
    if (!ss.id.empty()) S.idmap.get(fnv(ss.id.data(), ss.id.size()))->val.store(ss.sg, std::memory_order_release);
    ss.meta_pub.store(1, std::memory_order_release);
  }
  std::unordered_set<uint64_t> closed;
  for (const auto& t : sh.turns) closed.insert(t.o0);
  const TsP* open_ts = nullptr;
  for (const auto& x : sh.tss) if (!closed.count(x.off)) { open_ts = &x; break; }
  const uint64_t item_end = n ? I[n - 1].off + I[n - 1].len + 1 : 0;
  const uint64_t meta_end = meta ? meta->off + meta->len + 1 : 0;
  uint64_t start = std::max(item_end, meta_end);
  if (open_ts && open_ts->off < start) start = open_ts->off;
  size_t k = (size_t)(std::lower_bound(I.begin(), I.end(), start, [](const ItemP& a, uint64_t v) { return a.off < v; }) - I.begin());  // items before start
  for (size_t i = k; i < n; i++) ss.have_rec.insert(I[i].off);
  for (const auto& x : sh.tss) if (x.off >= start) ss.have_rec.insert(x.off);
  for (const auto& m : sh.metas) if (m.off >= start) ss.have_rec.insert(m.off);
  Sess::Base b;
  if (open_ts && open_ts->off == start) {
    if (open_ts->r > 0) { b.has = true; b.ts = open_ts->ts; b.r = open_ts->r - 1; b.th = b.ts + b.r; }
  } else if (n && start == item_end && item_end >= meta_end) {
    const ItemP& L = I[n - 1];
    b.has = true; b.ts = L.ts; b.r = L.r; b.th = L.ts + L.r;
  } else if (meta && start == meta_end) {
    b.has = true; b.ts = meta->ts; b.r = meta->r; b.th = meta->ts + meta->r;
  }
  uint64_t ordmax = 0;
  // items
  std::vector<uint32_t> ids(k);
  uint32_t urank = 0;
  for (size_t i = 0; i < k; i++) {
    ItemP& x = I[i];
    const uint32_t id = S.nit.fetch_add(1) + 1;
    Item& m = S.it.ensure(id);
    m.sess = ss.sg; m.len = x.len; m.tok = x.tok; m.pos = x.pos; m.ord = x.ord; m.off = x.off; m.cid = x.cid; m.ch = x.ch;
    m.th.store(x.ts + x.r, std::memory_order_relaxed); m.rs.store(x.rs, std::memory_order_relaxed);
    m.kind = x.kind; m.mode = x.mode; m.fwd = (x.flags & 1) != 0; m.parts = std::move(x.parts);
    if (m.cid) link_call(S, ss.sg, id);
    ss.sess.set(x.pos, id);
    if (m.kind == K_USER) ss.usr.set(urank++, id);
    ss.ot.add(x.ord, 1ull | (m.kind == K_USER ? (1ull << 32) : 0), 0);
    ordmax = std::max<uint64_t>(ordmax, (uint64_t)x.ord + 1);
    ids[i] = id;
  }
  S.restored_items.fetch_add(k, std::memory_order_relaxed);
  // the injection state: the injected items before `start` in their order; the run of injected lines right before `start` is still open
  size_t run_from = k;
  if (k > 0 && I[k - 1].kind == K_DEV && I[k - 1].off + I[k - 1].len + 1 == start) {
    run_from = k - 1;
    while (run_from > 0 && I[run_from - 1].kind == K_DEV && I[run_from - 1].off + I[run_from - 1].len + 1 == I[run_from].off) run_from--;
  }
  {
    InstrState* st = nullptr;
    for (size_t i = 0; i < run_from; i++) if (I[i].kind == K_DEV) { if (!st) st = new InstrState; st->put(S.it, ids[i]); }
    if (st) ss.ist.store(st, std::memory_order_release);
  }
  // starts
  uint32_t sa = 0; size_t tsi = 0;
  for (const auto& x : sh.tss) if (x.off < start) { ss.ot.add(x.ord, 0, 1); sa++; ordmax = std::max<uint64_t>(ordmax, (uint64_t)x.ord + 1); }
  if (meta) ordmax = std::max<uint64_t>(ordmax, (uint64_t)meta->rho + 1);
  // the lines of the open bundle keep their records, so that the line that closes it finds its members and the group of the line after it
  {
    uint32_t ua = 0; for (size_t i = 0; i < run_from; i++) if (I[i].kind == K_USER) ua++;
    uint32_t sai = 0; while (tsi < sh.tss.size() && sh.tss[tsi].off < (run_from < k ? I[run_from].off : start)) { tsi++; sai++; }
    for (size_t i = run_from; i < k; i++) {
      const uint32_t lid = S.nline.fetch_add(1) + 1;
      LineRec& L = S.lr.ensure(lid);
      L.off = I[i].off; L.len = I[i].len; L.ts = I[i].ts; L.r = I[i].r; L.th = I[i].ts + I[i].r; L.ord = I[i].ord; L.item = ids[i]; L.pfx = I[i].pfx;
      L.fl = LF_ITEM | LF_DEV; L.ia = uint32_t(i) + 1; L.ua = ua; L.sa = sai;
      L.done.store(1, std::memory_order_relaxed);
      S.lend.get(lk(ss.sg, L.off + L.len + 1))->val.store(lid, std::memory_order_release);
    }
  }
  // turns, their cue lists and their units
  uint32_t rt = 0;
  for (auto& tp : sh.turns) {
    if (tp.o0 >= start) continue;  // read again from the file
    bool okv = true; for (uint32_t ix : tp.viidx) if (ix >= k) okv = false;
    if (!okv) { S.shard_corrupt.fetch_add(1, std::memory_order_relaxed); break; }
    const uint32_t id = S.nturn.fetch_add(1) + 1;
    TurnRec* t = &S.TT.v.ensure(id);
    t->sg = ss.sg; t->o0 = tp.o0; t->o1 = tp.o1; t->ord0 = tp.ord0; t->ord1 = tp.ord1; t->pos0 = tp.pos0; t->pos1 = tp.pos1; t->th0 = tp.th0; t->the = tp.the; t->tq1 = tp.tq1;
    t->n = tp.n; t->src = tp.src; t->nu = tp.nu;
    t->vi.reserve(tp.viidx.size()); for (uint32_t ix : tp.viidx) t->vi.push_back(ids[ix]);
    t->C = std::move(tp.C); t->D = std::move(tp.D); t->len = std::move(tp.len); t->Q = std::move(tp.Q); t->Q1 = std::move(tp.Q1); t->Nc = std::move(tp.Nc);
    t->seed = ident_hash(ss) ^ (uint64_t(t->ord0) * 0x9E3779B97F4A7C15ull);  // D12
    t->tpos = rt;
    ss.tl.set(rt, id);
    for (uint64_t h : t->D) S.pt_add(ss.sg, h, t->tpos);
    S.B.insert(id, t->the);  // D3': in the order as it is restored
    if (const auto kr = sh.k5rec.find(tp.o0); kr != sh.k5rec.end()) k5_attach(*t, kr->second);  // K5 B-r7: restored units are candidates
    k5_turn_in(S, id);
    // the units in the shard are candidates: whether one of them is the unit of this turn is told by its fingerprint, in the first look after the load, when every turn
    // of every file is in 𝔅 (settle_one). Without a candidate the turn is counted.
    const auto c = sh.contribs.find(tp.o0);
    if (c != sh.contribs.end())
      for (ContribP& cp : c->second) {
        auto* q = new Contrib; q->phi = cp.phi; q->ny = cp.ny; q->root = cp.root; q->e = std::move(cp.e);
        q->next = t->cand; t->cand = q;
      }
    S.pend_total.fetch_add(1, std::memory_order_seq_cst);
    rt++;
  }
  ss.rturns = rt;
  b.ia = (uint32_t)k; b.sa = sa; { uint32_t ua = 0; for (size_t i = 0; i < k; i++) if (I[i].kind == K_USER) ua++; b.ua = ua; }
  b.ord = ordmax ? uint32_t(ordmax - 1) : 0;
  ss.base = b;
  ss.base_end = start;
  ss.front.store(start, std::memory_order_release); ss.pub.store(start, std::memory_order_release);
  ss.pubord.store(ordmax, std::memory_order_release);
}

// read the complete lines of the file from the front of the processed prefix and hand them in. P-3': a NUL byte at the position of a line means the writer has not
// put it there yet (positioned writes may leave NUL ranges for a while): live (watcher, request path) the read stops there and tries again at the next change
// event; at restore (`restore`) a range that is NUL only is skipped (a hole a dead process left) provided a complete line follows it, and counted in
// holes_skipped. The lines before the front are known; a line read twice is announced once.
void ingest_file_tail(Store& S, Sess& ss, bool restore = false) {
  if (ss.memmode) return;
  const auto source = source_fd(ss);
  if (!source) return;
  const int fd = source->fd;
  const uint64_t from = ss.front.load(std::memory_order_seq_cst);
  uint64_t st_size = 0;
  if (!vp_fsize(fd, st_size) || st_size <= from) return;
  vp_fadvise_seq(fd, from);
  VrsTailLease lease; auto& buf = lease.get();
  uint64_t base_off = from; size_t scan = 0, start = 0;
  const size_t CHUNK = size_t(1) << 24;
  bool nul_stop = false;
  while (!S.stop.load(std::memory_order_relaxed) && !nul_stop) {
    const size_t old = buf.size();
    const uint64_t pos = base_off + old;
    if (pos >= st_size && (!vp_fsize(fd, st_size) || pos >= st_size)) break;
    const size_t want = (size_t)std::min<uint64_t>(CHUNK, st_size - pos);
    buf.reserve_more(want);  // N-A+D: bound reads by observed bytes; initialize only bytes the OS writes
    const ssize_t got = vp_pread(fd, &buf[old], want, pos);
    if (got <= 0) break;
    buf.appended((size_t)got);
    for (;;) {
      if (start >= buf.size()) break;
      if (buf[start] == '\0') {  // not written yet, or a hole
        size_t p = start; while (p < buf.size() && buf[p] == '\0') p++;
        if (p == buf.size()) { scan = start; break; }                      // the run goes on: more bytes or the end of the file decide
        if (!restore) { nul_stop = true; break; }
        const size_t nl2 = buf.find('\n', p);
        if (nl2 == std::string::npos) { scan = start; break; }             // the line behind the run is not complete yet
        if (buf[p] != '{' || std::memchr(&buf[p], 0, nl2 - p) != nullptr) { nul_stop = true; break; }
        S.holes_skipped.fetch_add(1, std::memory_order_relaxed);
        S.holes.get(lk(ss.sg, base_off + start))->val.store(base_off + p, std::memory_order_release);
        S.holes_end.get(lk(ss.sg, base_off + p))->val.store(base_off + start, std::memory_order_release);
        start = p; scan = start; continue;
      }
      const size_t nl = buf.find('\n', scan > start ? scan : start);
      if (nl == std::string::npos) { scan = buf.size(); break; }
      if (std::memchr(&buf[start], 0, nl - start) != nullptr) { nul_stop = true; break; }  // a line with a hole inside: not complete
      insert_line(S, ss, base_off + start, &buf[start], nl - start, false);
      S.tail_lines.fetch_add(1, std::memory_order_relaxed);
      start = nl + 1; scan = start;
    }
    claim_and_run(S, ss);  // the lines of this piece are processed while the next is read
    if (start > 0) { buf.remove_prefix(start); base_off += start; scan -= start; start = 0; }
  }
  if (nul_stop) S.nul_stops.fetch_add(1, std::memory_order_relaxed);
  claim_and_run(S, ss);
}
void take_early(Store& S, Sess& ss) {  // lines handed in before the restore of their file was done
  EarlyNode* h = ss.early.exchange(nullptr, std::memory_order_seq_cst);
  while (h) { EarlyNode* nx = h->next; insert_line(S, ss, h->off, h->line.data(), h->line.size(), true); delete h; h = nx; }
}

void restore_file(Store& S, Sess& ss) {  // a worker that took the file number
  Shard sh;
  bool use = false;
  if (!S.idir.empty() && S.stale_index) {  // built under another spelling of the sessions directory: the seeds (hash of the path) differ
    if (vp_unlink(shard_path(S, ss).c_str()) == 0) S.shards_rebuilt.fetch_add(1, std::memory_order_relaxed);
  } else if (!S.idir.empty()) {
    load_shard(S, ss, sh);  // its mapping and read handle are both gone before any deletion below
    if (!sh.current) { if (vp_unlink(shard_path(S, ss).c_str()) == 0) S.shards_rebuilt.fetch_add(1, std::memory_order_relaxed); }
    else {
      S.shard_corrupt.fetch_add(sh.corrupt, std::memory_order_relaxed);
      if (sh.present) {
        if (shard_ahead(sh)) { S.shards_rebuilt.fetch_add(1, std::memory_order_relaxed); vp_unlink(shard_path(S, ss).c_str()); }  // records of lines that never reached the disk (P-3')
        else if (validate_shard(S, ss, sh)) { use = true; S.shards_loaded.fetch_add(1, std::memory_order_relaxed); }
        else { S.shards_rebuilt.fetch_add(1, std::memory_order_relaxed); vp_unlink(shard_path(S, ss).c_str()); }
      }
    }
  }
  if (use) materialize(S, ss, sh);
  ingest_file_tail(S, ss, true);  // [start, EOF): the bytes written since the shard was last appended to (or the whole file, no shard); NUL-only holes are skipped (P-3')
  ss.restored.store(1, std::memory_order_seq_cst);
  take_early(S, ss);
  ingest_file_tail(S, ss, false);  // what was written while the restore ran
}

void first_look(Store& S, uint32_t sg) {
  Sess& ss = S.sx(sg);
  ensure_segs(S, ss, ss.is_base);
  drive_counts(S, ss);
  if (ss.is_base && !ss.finished.exchange(1)) S.fin_left.fetch_sub(1);
  maybe_ready(S);
}
void run_task(Store& S, uint64_t v) {
  const uint32_t kind = uint32_t(v >> 56); const uint64_t id = v & ((uint64_t(1) << 56) - 1);
  S.tasks_done.fetch_add(1, std::memory_order_relaxed);
  switch (kind) {
    case T_RESTORE: {
      Sess& ss = *S.base[id];
      restore_file(S, ss);
      S.restore_left.fetch_sub(1);
      S.work.signal(true);  // every completion publishes the same availability notification
      return;
    }
    case T_FIRST: first_look(S, (uint32_t)id); return;
    case T_COUNT: run_count(S, reinterpret_cast<CountBatch*>(uintptr_t(id))); return;
    case T_REVISIT: run_revisit(S, reinterpret_cast<RevBatch*>(uintptr_t(id))); return;
    case T_INGEST: { Sess* s = S.sp[id].load(std::memory_order_acquire); if (s) { s->ing_q.store(0, std::memory_order_seq_cst); if (s->restored.load(std::memory_order_acquire)) ingest_file_tail(S, *s, false); } return; }
    case T_KICK: { Sess* s = S.sp[id].load(std::memory_order_acquire); if (s) ensure_segs(S, *s, false); return; }
    case T_K5: k5_run_task(S, id); return;
  }
}

// ---------------- sessions ----------------
std::string hex64(uint64_t v) { char b[17]; snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }
uint64_t fid_key(const std::string& path) {  // the id in the file name: the last 36 characters of the stem (rollout-<time>-<uuid>.jsonl); 0 = none
  const size_t sl = path.rfind('/');
  std::string_view f(path);
  if (sl != std::string::npos) f.remove_prefix(sl + 1);
  if (f.size() > 6 && f.compare(f.size() - 6, 6, ".jsonl") == 0) f.remove_suffix(6);
  if (f.size() < 36) return 0;
  const std::string_view id = f.substr(f.size() - 36);
  return fnv(id.data(), id.size());
}
Sess* make_sess(const std::string& path, const std::string& rel, const std::string& nu, uint32_t sg, bool base) {
  Sess* s = new Sess;
  s->sg = sg; s->path = path; s->rel = rel; s->nu = nu; s->root32 = uint32_t(fnv(rel.data(), rel.size()) & 0x7fffffffu);
  s->is_base = base;
  s->memmode = !base && !vp_exists(path.c_str());  // no file behind it: an ephemeral thread keeps its lines in memory (P-4)
  s->restored.store(base ? 0 : 1);
  return s;
}
// 6th stage D12: the identity of a file is the uuid in its name (fid_key), or its path relative to the store root; never the spelling of its path.
uint64_t ident_hash(const Sess& ss) {
  const uint64_t k = fid_key(ss.path);
  return k ? k : fnv(ss.rel.data(), ss.rel.size());
}
// P-14: wake every vrs_wait sleeper after a state change it may be waiting for
void bump_progress(Store& S) {
  S.progress.fetch_add(1, std::memory_order_seq_cst);
  vp_futex_wake_all(&S.progress);
}
// P-12: the path a caller hands in, as the store spells it: the real path of its directory plus its name (the file may not exist yet).
// A directory that does not exist yet: the real path of its longest existing ancestor plus the rest (what the directory will be once made,
// links are only ever in existing parts). Each thread keeps its own cache (no shared state), filled only once the whole directory exists.
std::string canon_path(const Store& S, const char* p) {
  (void)S;
  const std::string in(p);
#ifdef _WIN32  // KW2
  // Exact Store spelling already has P-12 identity. Aliases still take the realpath path below.
  for (const Root& r : S.roots) {  // B4: at most one root matches (R1)
    if (in.size() <= r.pre.size() || in.compare(0, r.pre.size(), r.pre) != 0) continue;
    const std::string nu = r.tag + in.substr(r.pre.size());
    if (const auto* nd = S.pathmap.find(fnv(nu.data(), nu.size()))) {
      const uint32_t sg = nd->val.load(std::memory_order_acquire);
      if (sg) if (Sess* ss = S.sp[sg].load(std::memory_order_acquire)) if (ss->path == in) return ss->path;
    }
    break;
  }
  using vp_cache_t = std::unordered_map<std::string, std::string>;
  vp_cache_t& cache = VP_TLS(vp_cache_t);
#else
  thread_local std::unordered_map<std::string, std::string> cache;
#endif
  if (cache.size() > 4096) cache.clear();
  const auto it = cache.find(in);
  if (it != cache.end()) return it->second;
  if (char* r = vp_realpath(in.c_str())) {  // the file exists: its one real path
    std::string out(r);
    std::free(r);
    cache.emplace(in, out);
    return out;
  }
  const size_t sl = in.rfind('/');
  if (sl == std::string::npos || sl == 0) return in;
  size_t cut = sl;  // in[0, cut) is the directory part being resolved; in[cut, end) is appended as is
  for (;;) {
    const std::string dir = in.substr(0, cut);
    if (char* r = vp_realpath(dir.c_str())) {
      std::string out = std::string(r) + in.substr(cut);
      std::free(r);
      return out;  // not cached: the file does not exist yet
    }
    const size_t up = in.rfind('/', cut - 1);
    if (up == std::string::npos || up == 0) return in;
    cut = up;
  }
}
// R2 (VRS_onePC_v1.md D-u1, B1): a path below the root with tag "" that would read as another root's name (or as "_ext/") is not a file of the store
bool name_clash(const Store& S, const std::string& rel) {
  if (rel.compare(0, 5, "_ext/") == 0) return true;
  for (const Root& r : S.roots) if (!r.tag.empty() && rel.compare(0, r.tag.size(), r.tag) == 0) return true;
  return false;
}
// B4: the root of p (R1: at most one), rel = its path below that root, nu = tag + rel; a path below no root is "_ext/…" as before. false = R2 refuses it.
bool store_name(const Store& S, const std::string& p, std::string& rel, std::string& nu) {
  for (const Root& r : S.roots) {
    if (p.size() <= r.pre.size() || p.compare(0, r.pre.size(), r.pre) != 0) continue;
    rel = p.substr(r.pre.size());
    if (r.tag.empty() && name_clash(S, rel)) return false;
    nu = r.tag + rel;
    return true;
  }
  const size_t sl = p.rfind('/'); rel = "_ext/" + hex64(fnv(p.data(), p.size())) + "_" + (sl == std::string::npos ? p : p.substr(sl + 1));
  nu = rel;
  return true;
}
Sess* get_sess(Store& S, const std::string& p, bool create) {
  std::string rel, nu;
  if (!store_name(S, p, rel, nu)) return nullptr;
  const uint64_t key = fnv(nu.data(), nu.size());
  if (const auto* nd = S.pathmap.find(key)) {
    const uint32_t v = nd->val.load(std::memory_order_acquire);
    if (v) { Sess* s; while (!(s = S.sp[v].load(std::memory_order_acquire))) { S.insert_waits.fetch_add(1, std::memory_order_relaxed); std::this_thread::yield(); } return s; }
  }
  if (!create) return nullptr;
  const uint32_t sg = S.nsg.fetch_add(1) + 1;
  if (sg > 65535) { S.nsg.fetch_sub(1); return nullptr; }
  Sess* ns = make_sess(p, rel, nu, sg, false);
  auto* nd = S.pathmap.get(key);
  uint32_t e = 0;
  if (nd->val.compare_exchange_strong(e, sg, std::memory_order_acq_rel)) {
    S.sp[sg].store(ns, std::memory_order_release);
    if (const uint64_t fk = fid_key(p)) S.fidmap.get(fk)->val.store(sg, std::memory_order_release);
    S.first.seed(sg - 1, task_of(T_FIRST, sg));
    S.work.signal(true);
    return ns;
  }
  delete ns;  // another thread made the session first; the number we took stays empty
  Sess* s; while (!(s = S.sp[e].load(std::memory_order_acquire))) { S.insert_waits.fetch_add(1, std::memory_order_relaxed); std::this_thread::yield(); }
  return s;
}
void ask_ingest(Store& S, Sess& ss) {  // the file has new bytes: a task reads them (one queued task per file)
  if (!ss.restored.load(std::memory_order_acquire)) return;  // the restore reads the file to its end
  if (ss.ing_q.exchange(1, std::memory_order_seq_cst)) return;
  if (!S.work.push(task_of(T_INGEST, ss.sg))) { ss.ing_q.store(0, std::memory_order_seq_cst); ingest_file_tail(S, ss, false); }
}

// ---------------- resident workers (P-2) and the file watcher (P-3) ----------------
// N-B: each worker independently selects an execution obligation. All initial
// session checks already exist; no completing worker distributes a later stage.
bool next_work(Store& S, uint64_t& v, uint32_t& hint) {
  if (S.work.pop(v, hint)) return true;
  if (S.restore_left.load(std::memory_order_seq_cst) == 0) {
    S.loaded.store(1, std::memory_order_seq_cst);
    k5_start(S);  // K5 B-r7: the base is built once every shard is in
    if (S.first.pop(v, hint)) return true;
    maybe_ready(S);
  }
  return false;
}
void worker_main(Store* Sp, unsigned wi) {
  Store& S = *Sp;
  if (!S.cpus.empty()) vp_pin_cpu(S.cpus[wi % S.cpus.size()]);
  uint32_t hint = wi * 11;
  for (;;) {
    if (S.stop.load(std::memory_order_relaxed)) break;
    uint64_t v;
    if (next_work(S, v, hint)) { run_task(S, v); continue; }
    const uint32_t sg0 = S.work.sig.load(std::memory_order_seq_cst);
    if (next_work(S, v, hint)) { run_task(S, v); continue; }
    if (S.stop.load(std::memory_order_seq_cst)) break;
    vp_aw_wait(S.work.sig, sg0); // unchanged wait: eligible work or shutdown notification
  }
}

template <class AddW>
void scan_new_dir(Store& S, const std::string& dir, AddW& addw) {  // a directory the watcher has just met: watch it and every directory below it, take the files in it
  addw(dir);
  std::error_code ec;
  for (vp_recursive_directory_iterator it(vp_io_path(dir), fs::directory_options::skip_permission_denied, ec), e; !ec && it != e; it.increment(ec)) {
    std::error_code e2;
    if (it->is_symlink(e2)) continue;  // P-12: a link is not a second name for the store
    if (it->is_directory(e2)) { addw(vp_u8(it->path())); continue; }  // the watch is in before the iterator lists the directory
    if (!it->is_regular_file(e2) || it->path().extension() != ".jsonl") continue;
    Sess* ss = get_sess(S, canon_path(S, vp_u8(it->path()).c_str()), true);  // P-12
    if (ss) ask_ingest(S, *ss);
  }
}
void recover_root(Store& S, const Root& r) {  // B4: the overflow of one root recovers that root only
  if (S.stop.load()) return;
  if (r.recover) { r.recover(r.recover_context); return; }
  auto visit = [&](const vw::Witness& w) {
    vw::sources(w, [&](const std::string& path) {
      const auto p = canon_path(S, path.c_str());
      if (p.size() <= r.pre.size() || p.compare(0, r.pre.size(), r.pre)) throw std::runtime_error("WITNESS_OUTSIDE_SESSIONS");
      if (Sess* ss = get_sess(S, p, true)) ask_ingest(S, *ss);
    });
  };
  vw::scan(r.change_markers, false, visit);
  vw::scan(r.writer_markers, true, visit);
}
void recover_overflow(Store& S) { for (const Root& r : S.roots) recover_root(S, r); }
#ifdef _WIN32
// the watcher on Windows (13.2): ReadDirectoryChangesW over the whole tree (the platform layer); a name is taken the way the inotify loop below takes it
// B4: one watcher per root
void watch_main(Store* Sp, size_t ri) {
  Store& S = *Sp;
  struct Ctx { Store* S; const Root* r; };
  Ctx c{Sp, &Sp->roots[ri]};
  auto on_path = [](void* cx, const std::string& path, bool dir) {
    Store& S = *static_cast<Ctx*>(cx)->S;
    auto addw = [](const std::string&) {};  // the tree watch already covers a new directory
    if (dir) { scan_new_dir(S, path, addw); return; }
    const size_t sl = path.rfind('/');
    const std::string name = sl == std::string::npos ? path : path.substr(sl + 1);
    if (name.size() < 7 || name.compare(name.size() - 6, 6, ".jsonl") != 0) return;
    Sess* ss = get_sess(S, canon_path(S, path.c_str()), true);  // P-12
    if (ss) ask_ingest(S, *ss);
  };
  auto on_overflow = [](void* cx) {  // G3: only outstanding witnesses, never the history tree.
    Store& S = *static_cast<Ctx*>(cx)->S;
    try { recover_root(S, *static_cast<Ctx*>(cx)->r); } catch (...) { S.ready.store(-1); bump_progress(S); }
  };
  vp_watch_tree(c.r->dir, S.stop_fd, &c, on_path, on_overflow);
}
#else
void watch_main(Store* Sp, size_t) {  // one root only on this platform (vrs_open refuses several: B4)
  Store& S = *Sp;
  const int fd = inotify_init1(IN_CLOEXEC);
  if (fd < 0) return;
  std::unordered_map<int, std::string> wd2dir;
  auto addw = [&](const std::string& dir) {
    std::string real = dir;
    if (char* r = ::realpath(dir.c_str(), nullptr)) { real = r; std::free(r); }  // P-12
    const int wd = inotify_add_watch(fd, real.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO);
    if (wd >= 0) wd2dir[wd] = real;
  };
  addw(S.sdir);
  {
    std::error_code ec;
    for (vp_recursive_directory_iterator it(S.sdir, fs::directory_options::skip_permission_denied, ec), e; !ec && it != e; it.increment(ec)) {
      std::error_code e2;
      if (it->is_symlink(e2)) continue;  // P-12
      if (it->is_directory(e2)) addw(it->path().string());
    }
  }
  alignas(inotify_event) char buf[1 << 16];
  pollfd fds[2] = {{fd, POLLIN, 0}, {S.stop_fd, POLLIN, 0}};
  for (;;) {
    if (::poll(fds, 2, -1) < 0) { if (errno == EINTR) continue; break; }
    if (fds[1].revents) break;
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n <= 0) continue;
    for (ssize_t i = 0; i < n;) {
      const inotify_event* ev = (const inotify_event*)(buf + i);
      i += sizeof(inotify_event) + ev->len;
      if (ev->mask & IN_Q_OVERFLOW) {
        try { recover_overflow(S); } catch (...) { S.ready.store(-1); bump_progress(S); }
        continue;
      }
      const auto d = wd2dir.find(ev->wd);
      if (d == wd2dir.end() || ev->len == 0) continue;
      const std::string name(ev->name), path = d->second + "/" + name;
      if (ev->mask & IN_ISDIR) { if (ev->mask & (IN_CREATE | IN_MOVED_TO)) scan_new_dir(S, path, addw); continue; }
      if (name.size() < 7 || name.compare(name.size() - 6, 6, ".jsonl") != 0) continue;
      Sess* ss = get_sess(S, canon_path(S, path.c_str()), true);  // P-12
      if (!ss) continue;
      ask_ingest(S, *ss);
    }
  }
  ::close(fd);
}
#endif

// ---------------- D3: the input of a thread ----------------
int assemble_impl(Store& S, const char* path, uint64_t B, char** json, size_t* jlen) {
  const auto t0 = std::chrono::steady_clock::now();
  *json = nullptr; *jlen = 0;
  const int rd = S.ready.load(std::memory_order_acquire);
  if (rd < 0) return -2;
  if (rd == 0) return 1;  // D5: not ready is an error, never a fallback
  if (!path || !*path || B == 0) return -1;
  Sess* ss = get_sess(S, canon_path(S, path), true);  // P-12
  if (!ss) return -3;
  if (!ss->is_base && !ss->memmode && ss->front.load(std::memory_order_acquire) == 0) ingest_file_tail(S, *ss, false);  // a file nobody inserted yet: read it once
  EbrGuard guard(S);  // the observers and A0 sets the answer is made of are not freed under it
  ensure_segs(S, *ss, false);
  Snap sn;
  if (!read_state(S, *ss, sn)) return 1;  // VI-11: the lineage cannot be told yet
  Asm A; bool lemma = false;
  {
    tl_lemma = false;
    static const std::vector<std::array<uint32_t, 4>>* const none = nullptr;
    const Segs* sg = ss->segs.load(std::memory_order_acquire);
    const std::vector<std::array<uint32_t, 4>> empty_segs;  // lifetime encloses this assembly only (VI-33)
    (void)none;
    Ctx C(S, *ss, sn, sn.segok && sg ? &sg->v : &empty_segs);
    A = C.assemble(ss->sg, sn.lastth, B);
    lemma = tl_lemma;
  }
  std::string out;
  int rc = 0;
  if (lemma) { out = "{\"ok\":false,\"error\":\"observer: lemma E violated\"}"; rc = -5; }
  else if (A.overflow) { out = "{\"ok\":false,\"error\":\"required input over budget\"}"; rc = 2; }  // E2-22
  else {
    auto lines = fetch(S, A.ids);
    uint64_t cb = 0; uint32_t chg = 0, drp = 0;
    residue(S, A.ids, lines, &cb, &chg, &drp);  // L1 v28 D6¹³: partial renders send their standing units only
    out = "{\"ok\":true,\"items\":[";
    bool first = true;
    for (size_t q = 0; q < lines.size(); q++) { if (lines[q].empty()) continue; if (!first) out += ','; first = false; out += payload_of(lines[q]); }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    out += "],\"tokens\":" + std::to_string(A.tokens) + ",\"us\":" + std::to_string((long long)us) + "}";
  }
  char* m = (char*)std::malloc(out.size() + 1);
  if (!m) return -4;
  std::memcpy(m, out.data(), out.size()); m[out.size()] = 0;
  *json = m; *jlen = out.size();
  return rc;
}
// VRS_MCP_codexapp_v1.md 7.2 (not a definition): the same assembly as assemble_impl, written out with the original place of every item.
// Each item is {"f": the file the item was read from, "o": its byte offset, "n": its length without the newline, "p": the payload as assemble_impl sends it}.
// Nothing chosen, counted or judged differs: the steps below are those of assemble_impl, only the output names the fields an Item already holds.
std::string loc_quote(const std::string& s) {
  static const char hx[] = "0123456789abcdef";
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20) { o += "\\u00"; o += hx[c >> 4]; o += hx[c & 15]; }
    else o += char(c);
  }
  return o + "\"";
}
int assemble_loc_impl(Store& S, const char* path, uint64_t B, char** json, size_t* jlen) {
  const auto t0 = std::chrono::steady_clock::now();
  *json = nullptr; *jlen = 0;
  const int rd = S.ready.load(std::memory_order_acquire);
  if (rd < 0) return -2;
  if (rd == 0) return 1;
  if (!path || !*path || B == 0) return -1;
  Sess* ss = get_sess(S, canon_path(S, path), true);
  if (!ss) return -3;
  if (!ss->is_base && !ss->memmode && ss->front.load(std::memory_order_acquire) == 0) ingest_file_tail(S, *ss, false);
  EbrGuard guard(S);
  ensure_segs(S, *ss, false);
  Snap sn;
  if (!read_state(S, *ss, sn)) return 1;
  Asm A; bool lemma = false;
  {
    tl_lemma = false;
    const Segs* sg = ss->segs.load(std::memory_order_acquire);
    const std::vector<std::array<uint32_t, 4>> empty_segs;
    Ctx C(S, *ss, sn, sn.segok && sg ? &sg->v : &empty_segs);
    A = C.assemble(ss->sg, sn.lastth, B);
    lemma = tl_lemma;
  }
  std::string out;
  int rc = 0;
  if (lemma) { out = "{\"ok\":false,\"error\":\"observer: lemma E violated\"}"; rc = -5; }
  else if (A.overflow) { out = "{\"ok\":false,\"error\":\"required input over budget\"}"; rc = 2; }
  else {
    auto lines = fetch(S, A.ids);
    uint64_t cb = 0; uint32_t chg = 0, drp = 0;
    residue(S, A.ids, lines, &cb, &chg, &drp);
    out = "{\"ok\":true,\"items\":[";
    bool first = true;
    for (size_t q = 0; q < lines.size(); q++) {
      if (lines[q].empty()) continue;
      if (!first) out += ','; first = false;
      const Item& m = S.it[A.ids[q]];
      out += "{\"f\":" + loc_quote(S.sx(m.sess).path) + ",\"o\":" + std::to_string(m.off) + ",\"n\":" + std::to_string(m.len) + ",\"p\":" + payload_of(lines[q]) + "}";
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    out += "],\"tokens\":" + std::to_string(A.tokens) + ",\"us\":" + std::to_string((long long)us) + "}";
  }
  char* m = (char*)std::malloc(out.size() + 1);
  if (!m) return -4;
  std::memcpy(m, out.data(), out.size()); m[out.size()] = 0;
  *json = m; *jlen = out.size();
  return rc;
}

#include "vrs_k5.cpp"
void k5_run_task(Store& S, uint64_t p) { k5_run(S, reinterpret_cast<K5Task*>(uintptr_t(p))); }

}  // namespace

// ================= the C API (P-5) =================
struct vrs_h { Store* S; };

extern "C" {

int vrs_open(const vrs_opts* o, vrs_h** out) {
  if (!o || !out || (!o->nroots && !o->sessions_dir) || (o->nroots && !o->roots)) return -1;
  *out = nullptr;
  std::unique_ptr<Store> sp(new Store);
  Store& S = *sp;
  S.k5 = k5_new();
#ifdef VRS_K5_RACE
  if (const char* rs = std::getenv("VRS_K5_RACE_SEED")) S.k5->race_seed = std::strtoull(rs, nullptr, 10);  // T3-1c only
#endif
  // B1: nroots == 0 is the one root of before, with tag ""
  const vrs_root legacy{"", o->sessions_dir, o->change_markers, o->writer_markers, o->recover, o->recover_context};
  const vrs_root* given = o->nroots ? o->roots : &legacy;
  const unsigned nr = o->nroots ? o->nroots : 1;
#ifndef _WIN32
  if (nr > 1) return -6;  // B4: the inotify watcher takes one root
#endif
  for (unsigned i = 0; i < nr; i++) {
    if (!given[i].dir) return -1;
    Root r; r.tag = given[i].tag ? given[i].tag : ""; r.dir = given[i].dir;
    while (r.dir.size() > 1 && r.dir.back() == '/') r.dir.pop_back();
    if (char* rp = vp_realpath(r.dir.c_str())) { r.dir = rp; std::free(rp); }  // P-12: the store root as the file system spells it
    r.pre = r.dir + "/";
    r.change_markers = given[i].change_markers ? given[i].change_markers : r.dir + ".marks";
    r.writer_markers = given[i].writer_markers ? given[i].writer_markers : r.dir + ".writers";
    r.recover = given[i].recover; r.recover_context = given[i].recover_context;
    if (!r.tag.empty() && (r.tag.back() != '/' || r.tag == "_ext/")) return -5;
    S.roots.push_back(std::move(r));
  }
  for (size_t i = 0; i < S.roots.size(); i++) for (size_t j = 0; j < S.roots.size(); j++) {  // R1, R2 (tags)
    if (i == j) continue;
    const Root& a = S.roots[i]; const Root& b = S.roots[j];
    if (b.pre.compare(0, a.pre.size(), a.pre) == 0) return -5;      // a's directory is b's or above it
    if (a.tag.empty() && b.tag.empty()) return -5;                    // at most one root with tag ""
    if (!a.tag.empty() && b.tag.compare(0, a.tag.size(), a.tag) == 0) return -5;
  }
  S.sdir = S.roots[0].dir;
  S.idir = o->index_dir ? o->index_dir : "";
  while (S.idir.size() > 1 && S.idir.back() == '/') S.idir.pop_back();
  if (!S.idir.empty()) { std::error_code ec; fs::create_directories(vp_io_path(S.idir), ec); if (char* r = vp_realpath(S.idir.c_str())) { S.idir = r; std::free(r); } }
#ifdef VRS_DBG_HOOKS
  if (const char* hv = std::getenv("VRS_DBG_HOLD_READY_MS")) S.hold_ready = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::atoll(hv));  // T6-2 only
  S.dbg_fail_ready = std::getenv("VRS_DBG_FAIL_READY") != nullptr;
#endif
  S.segwin = o->segwin != 0;
  S.budget = o->budget ? o->budget : 258400;
  S.nworkers = o->workers ? o->workers : std::max(1u, std::min(12u, std::thread::hardware_concurrency()));
  if (o->cpus) for (unsigned i = 0; i < o->ncpus; i++) S.cpus.push_back(o->cpus[i]);
  S.t_open = std::chrono::steady_clock::now();
  // B3: each root's tree is listed on its own (root 0 by this thread, every other root by a thread of its own, met once for the union)
  struct Found { std::string path, rel, nu; };
  std::vector<std::vector<Found>> per(S.roots.size());
  std::vector<int> per_rc(S.roots.size(), 0);
  auto list_root = [&](size_t ri) {
    const Root& r = S.roots[ri];
    std::error_code ec;
    if (!fs::is_directory(vp_io_path(r.dir), ec)) { per_rc[ri] = -2; return; }
    for (vp_recursive_directory_iterator it(vp_io_path(r.dir), ec), e; !ec && it != e; it.increment(ec)) {
      std::error_code e2;
      if (it->is_symlink(e2)) continue;  // P-12: a link is not a second name for the store
      if (it->is_regular_file(e2) && it->path().extension() == ".jsonl") {
#ifdef _WIN32
        const std::string p = vp_u8(vp_path(vp_u8(it->path()))); // Strip I/O namespace before path ordering.
#else
        const std::string p = it->path().string();
#endif
        Found f; f.path = p;
        f.rel = p.compare(0, r.pre.size(), r.pre) == 0 ? p.substr(r.pre.size()) : p;
        if (r.tag.empty() && name_clash(S, f.rel)) { per_rc[ri] = -5; return; }  // R2 (A-u1)
        f.nu = r.tag + f.rel;
        per[ri].push_back(std::move(f));
      }
    }
    if (ec) per_rc[ri] = -2;
  };
  {
    std::vector<std::thread> listers;
    for (size_t ri = 1; ri < S.roots.size(); ri++) listers.emplace_back(list_root, ri);
    list_root(0);
    for (auto& t : listers) t.join();
  }
  for (int rc : per_rc) if (rc) return rc;
  std::vector<Found> files;
  for (auto& v : per) for (auto& f : v) files.push_back(std::move(f));
  // 42f numbers the sessions in this order; B3: by the store name (one root: the order of the full paths, which share the root's prefix)
  std::sort(files.begin(), files.end(), [](const Found& a, const Found& b) { return vp_path(a.nu).compare(vp_path(b.nu)) < 0; });
  if (files.size() > 65000) files.resize(65000);
  if (!S.idir.empty()) {
    std::error_code ec; fs::create_directories(vp_io_path(S.idir), ec); if (ec) return -3;
    // the observers are seeded by the path of the rollout as it is spelled here, so the cell contributions in the shards are only valid for that spelling
    const std::string mf = S.idir + "/.sessions_dir";
    std::string old;
    { std::ifstream in(vp_io_path(mf), std::ios::binary); if (in) old.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
    std::string now = S.sdir;  // B2: one root as before; several: "tag TAB dir" per line (a record only)
    if (S.roots.size() > 1) { now.clear(); for (const Root& r : S.roots) now += r.tag + "\t" + r.dir + "\n"; }
    if (old != now) {
      S.stale_index = false;  // 6th stage D12: the seeds are the file ids, not path hashes; a moved or respelled root keeps its shards
      std::ofstream out(vp_io_path(mf), std::ios::binary | std::ios::trunc); out << now;
    }
  }
  for (size_t f = 0; f < files.size(); f++) {
    const std::string& p = files[f].path;
    const uint32_t sg = (uint32_t)f + 1;
    Sess* ss = make_sess(p, files[f].rel, files[f].nu, sg, true);
    S.pathmap.get(fnv(files[f].nu.data(), files[f].nu.size()))->val.store(sg);
    S.sp[sg].store(ss);
    if (const uint64_t fk = fid_key(p)) S.fidmap.get(fk)->val.store(sg);
    S.base.push_back(ss);
  }
  S.nsg.store((uint32_t)files.size());
  S.stop_fd = vp_stop_new();
  vrs_h* h = new vrs_h{sp.release()};
  Store* Sr = h->S;
  Sr->B.st = Sr; Sr->B.yields = &Sr->insert_waits;
  Sr->restore_left.store((uint32_t)S.base.size()); Sr->fin_left.store((int64_t)S.base.size());
  for (uint32_t i = 0; i < (uint32_t)S.base.size(); i++) {
    Sr->work.seed(i, task_of(T_RESTORE, i));
    Sr->first.seed(i, task_of(T_FIRST, S.base[i]->sg));
  }
  for (unsigned w = 0; w < Sr->nworkers; w++) Sr->workers.emplace_back(worker_main, Sr, w);
  for (size_t ri = 0; ri < Sr->roots.size(); ri++) Sr->watchers.emplace_back(watch_main, Sr, ri);  // B4: one per root
  *out = h;
  return 0;
}

int vrs_ready(vrs_h* h) { return h ? h->S->ready.load(std::memory_order_acquire) : -1; }

int vrs_refresh(vrs_h* h, const char* path) {
  if (!h || !path || !*path) return -1;
  Store& S = *h->S;
  if (!vp_exists(path)) return 0; // A zero-output transcript has no C(T).
  Sess* ss = get_sess(S, canon_path(S, path), true);
  if (!ss) return -2;
  ask_ingest(S, *ss); return 0;
}
int vrs_dbg_overflow(vrs_h* h) {
  if (!h) return -1;
  try { recover_overflow(*h->S); return 0; } catch (...) { return -2; }
}

int vrs_insert(vrs_h* h, const char* path, uint64_t off, const char* line, size_t len) {
  if (!h || !path || !*path || (!line && len)) return -1;
  Store& S = *h->S;
  if (len && line[len - 1] == '\n') len--;
  Sess* ss = get_sess(S, canon_path(S, path), true);  // P-12
  if (!ss) return -2;
  auto& trace = visibility_trace();
  trace.off = off; trace.end = off + len + 1; trace.insert_sg = ss->sg; trace.duplicate = 0;
  if (off < (uint64_t(1) << 40)) { // same supported offset domain as insert_line
    const uint64_t end = off + len + 1;
    uint64_t seen = ss->insert_end.load(std::memory_order_seq_cst);
    while (seen < end && !ss->insert_end.compare_exchange_weak(seen, end, std::memory_order_seq_cst)) {}
  }
  if (off + len + 1 <= ss->base_end) return 0;  // the key is in already (restored)
  if (const auto* nd = S.lmap.find(lk(ss->sg, off))) if (const uint32_t known = nd->val.load(std::memory_order_acquire)) { trace.duplicate = known == CLAIMING ? 1 : 2; return 0; }  // announced already (idempotent)
  if (!ss->memmode) uw_add(S, *ss, off, line, len);  // P-3': the bytes stay here until vrs_written; fetch reads them from here first
  // Nobody waits for anybody (§5.6 P-7). The thread that hands a line in announces it and takes over what is joined to the processed prefix; a thread that does
  // not win the CAS on the front finds that somebody else has the lines.
  if (!ss->restored.load(std::memory_order_seq_cst)) {  // the restore of this file has not set its prefix up: the line waits in a stack the restore empties
    EarlyNode* n = new EarlyNode{off, std::string(line ? line : "", len), nullptr};
    EarlyNode* head = ss->early.load(std::memory_order_relaxed);
    do { n->next = head; } while (!ss->early.compare_exchange_weak(head, n, std::memory_order_seq_cst, std::memory_order_relaxed));
    if (ss->restored.load(std::memory_order_seq_cst)) take_early(S, *ss);
    return 0;
  }
  insert_line(S, *ss, off, line ? line : "", len, true);
  return 0;
}

// P-3': the line at (path, off) is on disk now (written and flushed by the caller); the copy is let go. 0 ok, -1 bad argument, -2 unknown file,
// -3 no copy held for that key (never inserted through vrs_insert, or already released).
int vrs_written(vrs_h* h, const char* path, uint64_t off) {
  if (!h || !path || !*path) return -1;
  Store& S = *h->S;
  Sess* ss = get_sess(S, canon_path(S, path), false);  // P-12
  if (!ss) return -2;
  if (!uw_del(S, *ss, off)) { S.uw_unknown.fetch_add(1, std::memory_order_relaxed); return -3; }
  mark_wm(S, *ss);  // the confirmed prefix may have grown: put the new mark in the shard (one small O_APPEND write; the largest mark counts)
  return 0;
}

// P-14 + N-F: ready, lineage set up, and the explicitly supplied prefix published.
// The existing caller wait uses progress; advance_pub already notifies registered prefix waiters.
int vrs_wait(vrs_h* h, const char* path, uint64_t timeout_ms) {
  if (!h) return -1;
  Store& S = *h->S;
  const std::string cp = (path && *path) ? canon_path(S, path) : std::string();
  struct PrefixWaiter {
    std::atomic<uint32_t>& count;
    explicit PrefixWaiter(std::atomic<uint32_t>& c) : count(c) { count.fetch_add(1, std::memory_order_seq_cst); }
    ~PrefixWaiter() { count.fetch_sub(1, std::memory_order_seq_cst); }
  } waiter(S.pub_waiters);
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const uint32_t seen = S.progress.load(std::memory_order_seq_cst);
    const int rd = S.ready.load(std::memory_order_seq_cst);
    if (rd < 0) return -2;
    if (rd == 1) {
      if (cp.empty()) return 0;
      Sess* ss = get_sess(S, cp, false);
      if (!ss) return 0;  // a file the store has not met: the assembly makes it
      const uint64_t required = ss->insert_end.load(std::memory_order_seq_cst);
      if (ss->pub.load(std::memory_order_seq_cst) >= required &&
          (ensure_segs(S, *ss, false) || !(ss->meta_pub.load(std::memory_order_acquire) && !ss->fpar.empty()))) return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) return 1;
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(end - now).count();
    vp_futex_wait(&S.progress, seen, (int64_t)left);
  }
}
// VRS_MCP_libvrs_v1.md 13.2 / 14.4 KW3 (not a definition): wait until the processed prefix of the file covers byte `off`. 0 = it does, 1 = not within timeout_ms,
// -1 bad argument, -2 unknown file or failed store. The caller sleeps on the progress word; while somebody waits, a move of a processed prefix bumps it
// (advance_pub). It reads the state the library keeps (pub) and changes nothing.
int vrs_wait_off(vrs_h* h, const char* path, uint64_t off, uint64_t timeout_ms) {
  if (!h || !path || !*path) return -1;
  Store& S = *h->S;
  Sess* ss = get_sess(S, canon_path(S, path), false);
  if (!ss) return -2;
  S.pub_waiters.fetch_add(1, std::memory_order_seq_cst);
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int rc = 1;
  for (;;) {
    const uint32_t seen = S.progress.load(std::memory_order_seq_cst);
    if (ss->pub.load(std::memory_order_seq_cst) >= off) { rc = 0; break; }
    if (S.ready.load(std::memory_order_seq_cst) < 0) { rc = -2; break; }
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) break;
    vp_futex_wait(&S.progress, seen, (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - now).count());
  }
  S.pub_waiters.fetch_sub(1, std::memory_order_seq_cst);
  return rc;
}
int vrs_assemble_b(vrs_h* h, const char* path, uint64_t budget, char** json, size_t* len) {
  if (!h || !json || !len) return -1;
  return assemble_impl(*h->S, path, budget, json, len);
}
int vrs_assemble(vrs_h* h, const char* path, char** json, size_t* len) {
  if (!h || !json || !len) return -1;
  return assemble_impl(*h->S, path, h->S->budget, json, len);
}
int vrs_assemble_loc(vrs_h* h, const char* path, uint64_t budget, char** json, size_t* len) {
  if (!h || !json || !len) return -1;
  return assemble_loc_impl(*h->S, path, budget, json, len);
}
void vrs_free(char* p) { std::free(p); }

// ---- K5 3판 (VRS_K5_query_v1.md §14 B-r1 / B-r5)
int vrs_k5_ready(vrs_h* h) { return h && h->S->k5 ? h->S->k5->ready.load(std::memory_order_acquire) : -1; }
int vrs_search(vrs_h* h, const char* query, size_t qlen, const char* const* w, unsigned nw, char** json, size_t* len) {
  if (!h || !json || !len || (!query && qlen)) return -1;
  *json = nullptr; *len = 0;
  Store& S = *h->S;
  const int rd = S.ready.load(std::memory_order_acquire);
  if (rd < 0) return -2;
  if (rd == 0 || !S.k5->ready.load(std::memory_order_acquire)) return 1;  // B-r5 1: not ready is an error, never a fallback
  std::vector<std::string> W;
  for (unsigned i = 0; i < nw; i++) if (w && w[i] && *w[i]) W.push_back(w[i]);
  const std::string out = k5_search_json(S, std::string(query ? query : "", qlen), W);
  char* m = (char*)std::malloc(out.size() + 1);
  if (!m) return -4;
  std::memcpy(m, out.data(), out.size()); m[out.size()] = 0;
  *json = m; *len = out.size();
  return 0;
}
int vrs_recall(vrs_h* h, const char* query, size_t qlen, const char* const* w, unsigned nw, const char* caller, char** json, size_t* len) {
  if (!h || !json || !len || (!query && qlen)) return -1;
  *json = nullptr; *len = 0;
  Store& S = *h->S;
  const int rd = S.ready.load(std::memory_order_acquire);
  if (rd < 0) return -2;
  if (rd == 0 || !S.k5->ready.load(std::memory_order_acquire)) return 1;
  std::vector<std::string> W;
  for (unsigned i = 0; i < nw; i++) if (w && w[i] && *w[i]) W.push_back(w[i]);
  const std::string out = k5_recall_json(S, std::string(query ? query : "", qlen), W, caller ? caller : "");
  char* m = (char*)std::malloc(out.size() + 1);
  if (!m) return -4;
  std::memcpy(m, out.data(), out.size()); m[out.size()] = 0;
  *json = m; *len = out.size();
  return 0;
}
int vrs_k5_dump(vrs_h* h, const char* const* ids, const char* const* qs, unsigned nq, const char* file) {
  if (!h || !file || (nq && (!ids || !qs))) return -1;
  Store& S = *h->S;
  if (S.ready.load() != 1 || !S.k5->ready.load()) return 1;
  std::vector<std::pair<std::string, std::string>> q;
  for (unsigned i = 0; i < nq; i++) q.push_back({ids[i], qs[i]});
  return k5_dump(S, q, file);
}
void vrs_k5_stats(vrs_h* h, vrs_k5dbg* d) {
  if (!h || !d) return;
  std::memset(d, 0, sizeof *d);
  const K5State& k = *h->S->k5;
  d->ready = k.ready.load(); d->base = k.base_pub.load(); d->pend = k.pend.load(); d->ready_us = k.ready_us.load();
  d->units_built = k.units_built.load(); d->units_restored = k.units_restored.load(); d->units_checked = k.units_checked.load();
  d->revisits = k.revisits.load(); d->rev_tasks = k.rev_tasks.load(); d->shadow_n = k.shadow_n.load(); d->shadow_bad = k.shadow_bad.load();
  d->list_entries = k.list_entries.load(); d->node_bytes = k.node_bytes.load(); d->unit_bytes = k.unit_bytes.load();
  const K5Base* b = k.base.load(); d->base_bytes = b ? b->bytes : 0;
  uint64_t be = 0; if (b) for (int i = 0; i < 64; i++) be += b->kp[i].id.size() + b->qp[i].id.size();
  d->base_entries = be;
  d->syn_bytes = k.syn_bytes.load(); d->syn_miss = k.syn_miss.load();
  d->size = be * 4 + d->node_bytes + d->unit_bytes + d->syn_bytes;  // T3-9: list elements x 4 + skip-list nodes + units + synapse slots
  d->claimed = k.claimed.size(); d->retries = h->S->count_retries.load();
  EbrGuard guard(*h->S);
  for (uint32_t i = 1, e = h->S->nturn.load(); i <= e; i++) {  // T3-9′ (i): the exposures of each closed turn, Σ_e |hs_e| <= 64·|Q(x)|
    TurnRec* t = tt_find(*h->S, i);
    if (!t) continue;
    const K5C* c = t->k5c.load();
    if (!c) continue;
    d->ex_total += c->hs.size();
    if (c->hs.size() > uint64_t(K5F) * t->Q.size()) d->ex_bad++;
  }
}
// T3-1s at rest: every cell is the sum of the units installed now; 4판: every synapse is the sum of the shares of the direct items installed now
int vrs_k5_check_cells(vrs_h* h) {
  if (!h) return -1;
  Store& S = *h->S;
  EbrGuard guard(S);
  int64_t an[K5NC] = {}, ab[K5NC] = {};
  std::map<std::pair<int, uint32_t>, int64_t> rc;
  struct SV { int64_t an = 0, ab = 0; std::map<uint32_t, int64_t> rc; };
  std::map<std::pair<uint64_t, uint32_t>, SV> sv;
  for (uint32_t i = 1, e = S.nturn.load(); i <= e; i++) {
    TurnRec* t = tt_find(S, i);
    if (!t) continue;
    const K5C* c = t->k5c.load();
    if (!c) continue;
    for (const K5CE& x : c->e) {
      const bool a = x.fl & 1, b = (x.fl & 2) != 0;
      const int64_t dan = a ? ((int64_t(1) << 32) + 1) : 1, dab = a != b ? (a ? (int64_t(1) << 32) : 1) : 0;
      an[x.c] += dan; ab[x.c] += dab;
      rc[{x.c, c->root}]++;
      for (uint32_t k = 0; k < x.nh; k++) { SV& v = sv[{t->Q[c->hs[x.h0 + k]], x.y}]; v.an += dan; v.ab += dab; v.rc[c->root]++; }
    }
    for (const K5CI& x : c->ci) {
      an[x.k] += (int64_t(x.a) << 32) + int64_t(x.n); ab[x.k] += (int64_t(x.a2) << 32) + int64_t(x.b2);
      rc[{x.k, c->root}] += x.n;
    }
  }
  int bad = 0;
  for (int k = 0; k < K5NC; k++) {
    uint32_t roots = 0; for (const auto& [key, v] : rc) if (key.first == k && v > 0) roots++;
    if ((uint64_t)an[k] != S.k5->cells[k].an.load() || (uint64_t)ab[k] != S.k5->cells[k].ab.load() || roots != S.k5->cells[k].lin1.load()) bad++;
  }
  uint64_t held = 0;  // the slots with n > 0 must be exactly the synapses summed above
  for (const auto& [key, v] : sv) {
    const K5Syn* cl = k5_syn(S, key.first, key.second, false);
    uint32_t roots = 0; for (const auto& [r, n] : v.rc) if (n > 0) roots++;
    if (!cl || cl->an.load() != (uint64_t)v.an || cl->ab.load() != (uint64_t)v.ab || cl->lin.load() != roots) bad++;
  }
  if (const K5Base* B = S.k5->base.load())
    for (int pi = 0; pi < 64; pi++) for (uint32_t j = 0; j < B->kp[pi].id.size(); j++) if (B->kp[pi].syn[j].an.load() & 0xffffffffu) held++;
  for (size_t d = 0; d < LFMap<K5Head>::ND; d++) {
    auto* ch = S.k5->ok.dir[d].load();
    if (!ch) continue;
    for (size_t i = 0; i < LFMap<K5Head>::CHN; i++)
      for (auto* n = ch[i].load(); n; n = n->next.load()) {
        const K5Node* s = n->val.sent.load();
        for (const K5Node* x = s ? s->nx[0].load() : nullptr; x; x = x->nx[0].load())
          if (const K5Syn* cl = x->syn.load()) if (cl->an.load() & 0xffffffffu) held++;
      }
  }
  uint64_t want = 0; for (const auto& [key, v] : sv) if (v.an & 0xffffffff) want++;
  if (held != want) bad++;
  if (S.k5->syn_miss.load()) bad++;
  return bad;
}

void vrs_close(vrs_h* h) {
  if (!h) return;
  Store& S = *h->S;
  S.stop.store(1);
  S.work.sig.fetch_add(1);
  vp_aw_notify_all(S.work.sig);
  if (S.stop_fd >= 0) vp_stop_set(S.stop_fd);
  for (auto& t : S.workers) if (t.joinable()) t.join();
  for (auto& w : S.watchers) if (w.joinable()) w.join();
  delete h->S;
  delete h;
}

const char* vrs_dbg_fold_msg(vrs_h* h) { (void)h; return ""; }
void vrs_dbg_visibility(vrs_visibility* out) { if (out) *out = visibility_trace(); }
void vrs_dbg_stats(vrs_h* h, vrs_dbg* d) {
  if (!h || !d) return;
  Store& S = *h->S;
  std::memset(d, 0, sizeof *d);
  d->items = S.nit.load(); d->turns = S.nturn.load(); d->sessions = S.nsg.load(); d->lines_applied = S.lines_applied.load(); d->tail_lines = S.tail_lines.load();
  d->restored_items = S.restored_items.load();
  uint64_t hh = 1469598103934665603ull, cn = 0;
  for (int k = 0; k < NCA; k++) {
    const uint64_t a = S.cells[k].an.load(), b = S.cells[k].ab.load(), l1 = S.cells[k].lin1.load(), l2 = S.cells[k].lin1.load() >= 2;
    if (k < NC2) cn += a & 0xffffffffu;
    // which lineage root came first is a race; only none / one / two-or-more roots is observable
    for (uint64_t v : {a, b, (uint64_t)(l1 != 0), l2}) { hh ^= v + 0x9E3779B97F4A7C15ull + (hh << 6) + (hh >> 2); hh *= 1099511628211ull; }
  }
  d->cells_n = cn; d->cells_hash = hh;
  d->theta_checked = S.dbg_checked.load(); d->theta_mismatch = S.dbg_mismatch.load(); d->theta_decreasing = S.dbg_dec.load(); d->theta_maxgroup = S.dbg_maxgroup.load();
  d->shards_loaded = S.shards_loaded.load(); d->shards_rebuilt = S.shards_rebuilt.load(); d->shard_corrupt = S.shard_corrupt.load(); d->shard_err = S.shard_err.load();
  d->lemma_e = S.lemmaE.load(); d->ready_us = S.ready_us.load();
  d->tasks = S.tasks_done.load(); d->lin_waits = S.lin_waits.load();
  d->uw_lines = (uint64_t)std::max<int64_t>(0, S.uw_n.load()); d->uw_bytes = (uint64_t)std::max<int64_t>(0, S.uw_bytes.load());
  d->uw_added = S.uw_added.load(); d->uw_released = S.uw_released.load(); d->uw_unknown = S.uw_unknown.load();
  d->holes_skipped = S.holes_skipped.load(); d->nul_stops = S.nul_stops.load(); d->insert_waits = S.insert_waits.load();
  d->late_events = S.late_events.load(); d->reval_checked = S.reval_checked.load(); d->recounts = S.recounts.load();
  d->count_retries = S.count_retries.load(); d->ctr_n = S.ctr_n.load(); d->ctr_bytes = S.ctr_bytes.load(); d->obs_rebuilt = S.obs_rebuilt.load(); d->a0_rebuilt = S.a0_rebuilt.load();
  d->dep_pairs = S.dep_pairs.load(); d->dep_viol = S.dep_viol.load(); d->retired_n = S.retired_n.load(); d->retired_bytes = S.retired_bytes.load(); d->freed_n = S.freed_n.load(); d->settle_ok = S.settle_ok.load(); d->settle_bad = S.settle_bad.load();
  d->b_total = S.B.total.load(); d->b_miss = S.B.miss.load(); d->pend = (uint64_t)std::max<int64_t>(0, S.pend_total.load());
  d->bad_lines = S.bad_lines.load(); d->runs = S.runs.load(); d->run_max = S.run_max.load(); d->y_max = S.y_max.load(); d->x_max = S.x_max.load(); d->y_total = S.y_total.load();
  d->x_total = S.x_total.load(); d->revisit_tasks = S.revisit_tasks.load(); d->pass_retries = S.pass_retries.load(); d->dup_starts = S.dup_starts.load(); d->lines_in = S.lines_in.load();
}

int vrs_dbg_border_stat(vrs_h* h, uint64_t* days, uint64_t* maxn, uint64_t* total) {  // buckets in use, the largest bucket, turns in the order
  if (!h || !days || !maxn || !total) return -1;
  Store& S = *h->S;
  *days = *maxn = 0; *total = S.B.total.load();
  for (uint32_t d = 0; d < BDAYS; d++) if (const Bkt* b = S.B.bk[d].load(std::memory_order_acquire)) { (*days)++; *maxn = std::max<uint64_t>(*maxn, b->n); }
  return 0;
}
int vrs_dbg_dump_cells(vrs_h* h, const char* file) {
  if (!h || !file) return -1;
  Store& S = *h->S;
  FILE* f = vp_fopen(file, "w");
  if (!f) return -2;
  for (int k = 0; k < NCA; k++) {
    const uint64_t a = S.cells[k].an.load(), b = S.cells[k].ab.load(), l1 = S.cells[k].lin1.load(), l2 = S.cells[k].lin1.load() >= 2;
    if (a || b || l1 || l2) std::fprintf(f, "%d %llu %llu %d %llu\n", k, (unsigned long long)a, (unsigned long long)b, l1 != 0, (unsigned long long)l2);
  }
  std::fclose(f);
  return 0;
}
// hash of what the observer of every closed turn of one session is made of: the window (k, size), the turns (by path and ord0, not by number) at its reference positions,
// the rule parameters. Equal for two stores exactly when the observers are equal.
int vrs_dbg_obs_hash(vrs_h* h, const char* path, uint64_t* out) {
  if (!h || !path || !out) return -1;
  Store& S = *h->S;
  EbrGuard guard(S);
  Sess* ss = get_sess(S, canon_path(S, path), false);  // P-12
  if (!ss) return -2;
  uint64_t hh = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { hh ^= v + 0x9E3779B97F4A7C15ull + (hh << 6) + (hh >> 2); hh *= 1099511628211ull; };
  const PubView pv = pub_view(S, *ss);
  for (size_t i = 0, n = pv.ntl; i < n; i++) {
    const uint32_t y = ss->tl[i];
    const ObsY* o = obs_of(S, y);
    const Win w = window_of(S, *S.TT[y]);
    mix(S.TT[y]->seed); mix(o->wn); mix(o->bad); mix(o->rules ? 1 : 0); mix(o->m0); mix(o->max); mix(o->med);
    uint64_t t = 0; std::memcpy(&t, &o->tau, sizeof t); mix(t);
    for (uint32_t p : o->R) mix(S.TT[S.B[w.lo + p]]->seed);
  }
  *out = hh;
  return 0;
}


int vrs_dbg_obs_dump(vrs_h* h, const char* path, const char* file) {  // per closed turn of one session: the observer fields and the seeds of its reference turns
  if (!h || !path || !file) return -1;
  Store& S = *h->S;
  EbrGuard guard(S);
  Sess* ss = get_sess(S, canon_path(S, path), false);  // P-12
  if (!ss) return -2;
  FILE* f = vp_fopen(file, "w");
  if (!f) return -3;
  const PubView pv = pub_view(S, *ss);
  for (size_t i = 0, n = pv.ntl; i < n; i++) {
    const uint32_t y = ss->tl[i];
    const ObsY* o = obs_of(S, y);
    const Win w = window_of(S, *S.TT[y]);
    uint64_t rh = 0; for (uint32_t p : o->R) rh = rh * 1099511628211ull + S.TT[S.B[w.lo + p]]->seed;
    uint64_t t = 0; std::memcpy(&t, &o->tau, sizeof t);
    std::fprintf(f, "%zu seed=%llx the=%llu k=%u wn=%u lo=%u phi=%llx bad=%d rules=%d m0=%u max=%u med=%u tau=%llx R=%llx\n", i, (unsigned long long)S.TT[y]->seed, (unsigned long long)S.TT[y]->the, o->k, o->wn, (unsigned)w.lo, (unsigned long long)o->phi, (int)o->bad, o->rules ? 1 : 0, o->m0, o->max, o->med, (unsigned long long)t, (unsigned long long)rh);
  }
  std::fclose(f);
  return 0;
}


int vrs_dbg_unit_dump(vrs_h* h, const char* file) {  // every turn: its seed, the size of its Y, the fingerprint of its set and a hash of its unit
  if (!h || !file) return -1;
  Store& S = *h->S;
  EbrGuard guard(S);
  FILE* f = vp_fopen(file, "w");
  if (!f) return -3;
  const uint32_t n = S.nturn.load();
  for (uint32_t id = 1; id <= n; id++) {
    const TurnRec* t = S.TT[id];
    if (!t) continue;
    const Contrib* c = t->ctr.load(std::memory_order_acquire);
    uint64_t hh = 1469598103934665603ull;
    if (c) for (const CEnt& e : c->e) { hh ^= (uint64_t(e.cell) << 32) ^ e.cnt ^ (uint64_t(e.fl) << 48); hh *= 1099511628211ull; }
    const A0Val* a0 = t->a0.load(std::memory_order_acquire);
    uint64_t av = 0, ab = 0; size_t nv = 0, nb = 0;
    if (a0) { for (uint32_t y : a0->v) av += ci_mix(S.TT[y]->seed ^ S.TT[y]->o0); for (uint32_t y : a0->Yb) ab += ci_mix(S.TT[y]->seed ^ S.TT[y]->o0); nv = a0->v.size(); nb = a0->Yb.size(); }
    std::fprintf(f, "%016llx %llu ny=%llu phi=%016llx ne=%zu h=%016llx a0v=%zu/%016llx a0yb=%zu/%016llx\n", (unsigned long long)t->seed, (unsigned long long)t->o0, c ? (unsigned long long)c->ny : 0ull, c ? (unsigned long long)c->phi : 0ull, c ? c->e.size() : (size_t)0, (unsigned long long)hh, nv, (unsigned long long)av, nb, (unsigned long long)ab);
  }
  std::fclose(f);
  return 0;
}
// order-sensitive hash of everything vrs_insert/restore put into the tables for one session (items, turns), independent of item numbers
int vrs_dbg_session_hash(vrs_h* h, const char* path, uint64_t* out) {
  if (!h || !path || !out) return -1;
  Store& S = *h->S;
  Sess* ss = get_sess(S, canon_path(S, path), false);  // P-12
  if (!ss) return -2;
  uint64_t hh = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { hh ^= v + 0x9E3779B97F4A7C15ull + (hh << 6) + (hh >> 2); hh *= 1099511628211ull; };
  const PubView pv = pub_view(S, *ss);
  for (size_t i = 0, n = pv.ia; i < n; i++) {
    const Item& m = S.it[ss->sess[i]];
    mix(m.off); mix(m.len); mix(m.th); mix(m.tok); mix(m.kind); mix(m.pos); mix(m.ord); mix(m.cid); mix(m.ch); mix(m.rs); mix(m.fwd); mix(m.mode);
    for (const auto& p : m.parts) { mix(p.subj); mix(p.mode); mix(p.unit); for (uint64_t k : p.keys) mix(k); }
  }
  for (size_t i = 0, n = pv.ntl; i < n; i++) {
    const TurnRec& t = *S.TT[ss->tl[i]];
    mix(t.o0); mix(t.ord0); mix(t.ord1); mix(t.pos0); mix(t.pos1); mix(t.th0); mix(t.the); mix(t.tq1); mix(t.n); mix(t.src); mix(t.nu); mix(t.seed);
    for (uint32_t z : t.vi) mix(S.it[z].off);
    for (uint64_t v : t.C) mix(v);
    for (uint64_t v : t.D) mix(v);
    for (uint32_t v : t.len) mix(v);
    for (uint64_t v : t.Q) mix(v);
    for (uint64_t v : t.Q1) mix(v);
    for (uint64_t v : t.Nc) mix(v);
  }
  mix(pv.P); mix(pv.ia); mix(pv.ntl);
  *out = hh;
  return 0;
}

}  // extern "C"
