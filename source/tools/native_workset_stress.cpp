// Finite test workload: one resident group, unordered obligations, exact identities.
#include "vrs_workset.h"
#include <cstdio>
#include <thread>
#include <vector>
int main() {
  constexpr unsigned initial = 65536, threads = 12, each = 100000;
  constexpr unsigned total = initial + threads * each;
  auto counts = std::make_unique<std::atomic<unsigned>[]>(total);
  auto set = std::make_unique<VrsWorkSet>();
  std::atomic<unsigned> bad{0}, fallback{0};
  auto consume = [&](uint64_t v) {
    if (v >= total) { bad.fetch_add(1); return; }
    if (counts[v].fetch_add(1) != 0) bad.fetch_add(1);
  };
  for (unsigned i = 0; i < initial; ++i) set->seed(i, i);
  std::vector<std::thread> group;
  for (unsigned t = 0; t < threads; ++t) group.emplace_back([&, t] {
    uint32_t hint = t * 7;
    for (unsigned i = 0; i < each; ++i) {
      const uint64_t task = initial + t * each + i;
      if (!set->push(task)) { fallback.fetch_add(1); consume(task); }
      uint64_t got;
      if (set->pop(got, hint)) consume(got);
    }
    uint64_t got;
    while (set->pop(got, hint)) consume(got);
  });
  for (auto& t : group) t.join(); // test teardown only
  unsigned missing = 0;
  for (unsigned i = 0; i < total; ++i) if (counts[i].load() != 1) ++missing;
  std::printf("tasks=%u missing_or_duplicate=%u bad=%u inline_fallback=%u\n", total, missing, bad.load(), fallback.load());
  return missing || bad.load() ? 1 : 0;
}
