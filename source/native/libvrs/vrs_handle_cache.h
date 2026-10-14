// N-I: fixed, thread-local direct mapping; leases never enter work/file lists.
#pragma once
#include "vrs_plat.h"
#include <array>
#include <memory>

struct VrsFileIdentity {
  uint64_t volume = 0, low = 0, high = 0;
  bool operator==(const VrsFileIdentity&) const = default;
};
bool vrs_file_identity(int fd, VrsFileIdentity& result);

struct VrsHandleKey {
  std::atomic<const VrsFileIdentity*> identity{nullptr};
  ~VrsHandleKey() { delete identity.load(std::memory_order_relaxed); }
  bool accepts(int fd) {
    VrsFileIdentity value;
    if (!vrs_file_identity(fd, value)) return false;
    const VrsFileIdentity* prior = identity.load(std::memory_order_acquire);
    if (!prior) {
      auto candidate = std::make_unique<VrsFileIdentity>(value);
      if (identity.compare_exchange_strong(prior, candidate.get(), std::memory_order_acq_rel)) {
        candidate.release(); return true;
      }
    }
    return *prior == value;
  }
};

template <class File> class VrsHandleCache {
  struct Cell { std::shared_ptr<VrsHandleKey> key; std::shared_ptr<File> file; };
  std::array<Cell, 8> cells_;
public:
  static constexpr size_t capacity = 8;
  static size_t slot(const VrsHandleKey* key) {
    uint64_t h = uint64_t(reinterpret_cast<uintptr_t>(key));
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31; return size_t(h) % capacity;
  }
  static VrsHandleCache& local() {
#ifdef _WIN32
    return VP_TLS(VrsHandleCache);
#else
    thread_local VrsHandleCache cache;
    return cache;
#endif
  }
  template <class Open> std::shared_ptr<File> get(const std::shared_ptr<VrsHandleKey>& key, Open&& open) {
    Cell& cell = cells_[slot(key.get())];
    if (cell.key == key) return cell.file;
    // An outstanding caller retains its own reference through nested eviction.
    cell.file.reset(); cell.key.reset();
    auto file = open();
    if (file) { cell.key = key; cell.file = file; }
    return file;
  }
};

struct VrsFd {
  const int fd;
  explicit VrsFd(int value) : fd(value) {}
  ~VrsFd() { if (fd >= 0) vp_close(fd); }
  VrsFd(const VrsFd&) = delete;
};
using VrsFdLease = std::shared_ptr<VrsFd>;
