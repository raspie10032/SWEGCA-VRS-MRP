#ifndef VRS_WORKSET_H
#define VRS_WORKSET_H
#include "vrs_plat.h"
#include <atomic>
#include <cstdint>
#include <memory>

// N-B: unordered execution obligations; no arrival index or global head/tail.
// The fixed bounds, publication proof and collision fallback are in NATIVE_B.md.
class VrsWorkSet {
  static constexpr uint32_t CAP = 1u << 18;
  struct alignas(64) Shard {
    std::atomic<uint64_t> summary{0};
    std::atomic<uint64_t> bits[64]{};
  };
  std::unique_ptr<std::atomic<uint64_t>[]> values{new std::atomic<uint64_t>[CAP]()};
  Shard shards[64];
  static uint64_t mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }
  void mark(uint32_t p) {
    Shard& s = shards[p >> 12];
    const unsigned word = (p >> 6) & 63;
    s.bits[word].fetch_or(1ULL << (p & 63), std::memory_order_release);
    s.summary.fetch_or(1ULL << word, std::memory_order_release);
  }
  bool take(uint32_t& p, uint32_t& hint) {
    for (unsigned k = 0; k < 64; ++k) {
      const unsigned sh = (hint + k) & 63;
      Shard& s = shards[sh];
      uint64_t summary = s.summary.load(std::memory_order_acquire);
      while (summary) {
        const unsigned word = __builtin_ctzll(summary);
        const uint64_t wb = 1ULL << word;
        summary &= ~wb;
        if (!(s.summary.fetch_and(~wb, std::memory_order_acq_rel) & wb)) continue;
        const uint64_t bits = s.bits[word].exchange(0, std::memory_order_acq_rel);
        if (!bits) continue;
        const unsigned bit = __builtin_ctzll(bits);
        const uint64_t rest = bits & ~(1ULL << bit);
        if (rest) {
          s.bits[word].fetch_or(rest, std::memory_order_release);
          s.summary.fetch_or(wb, std::memory_order_release);
        }
        p = (sh << 12) | (word << 6) | bit;
        hint = (sh + 1) & 63;
        return true;
      }
    }
    return false;
  }
public:
  alignas(64) std::atomic<uint32_t> sig{0};
  // Private handle initialization only. Odd multiplication permutes CAP slots.
  void seed(uint32_t index, uint64_t task) {
    const uint32_t p = (index * 0x9e3779b9U) & (CAP - 1);
    values[p].store(task + 1, std::memory_order_relaxed);
    mark(p);
  }
  void signal(bool all = false) {
    sig.fetch_add(1, std::memory_order_seq_cst);
    if (all) vp_aw_notify_all(sig); else vp_aw_notify_one(sig);
  }
  bool push(uint64_t task) {
    const uint64_t h = mix(task);
    uint32_t p = uint32_t(h) & (CAP - 1);
    const uint32_t step = uint32_t(h >> 32) | 1U;
    for (unsigned k = 0; k < 64; ++k, p = (p + step) & (CAP - 1)) {
      uint64_t empty = 0;
      if (values[p].compare_exchange_strong(empty, task + 1, std::memory_order_acq_rel)) {
        mark(p); signal(); return true;
      }
    }
    return false; // caller executes this obligation itself, as before
  }
  bool pop(uint64_t& task, uint32_t& hint) {
    uint32_t p;
    while (take(p, hint)) {
      const uint64_t value = values[p].exchange(0, std::memory_order_acq_rel);
      if (value) { task = value - 1; return true; }
    }
    return false;
  }
};
#endif
