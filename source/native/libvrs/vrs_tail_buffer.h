// N-D storage only: valid bytes survive growth; reserved bytes are never read.
#pragma once
#include "vrs_plat.h"
#include <limits>
#include <memory>
#include <stdexcept>

class VrsTailBuffer {
  std::unique_ptr<char[]> bytes_;
  size_t size_ = 0, capacity_ = 0;
public:
  static constexpr size_t npos = size_t(-1);
  size_t size() const { return size_; }
  char& operator[](size_t i) { return bytes_[i]; }
  void clear() { size_ = 0; }
  void reserve_more(size_t extra) {
    if (extra > std::numeric_limits<size_t>::max() - size_) throw std::length_error("tail buffer");
    const size_t need = size_ + extra;
    if (need <= capacity_) return;
    const size_t grow = capacity_ <= std::numeric_limits<size_t>::max() / 2 ? capacity_ * 2 : need;
    const size_t cap = std::max(need, grow);
    std::unique_ptr<char[]> next(new char[cap]);
    if (size_) std::memcpy(next.get(), bytes_.get(), size_);
    bytes_ = std::move(next);
    capacity_ = cap;
  }
  void appended(size_t count) { size_ += count; }  // only OS-written bytes, <= reserved space
  size_t find(char c, size_t from) const {
    if (from >= size_) return npos;
    const void* p = std::memchr(bytes_.get() + from, c, size_ - from);
    return p ? size_t(static_cast<const char*>(p) - bytes_.get()) : npos;
  }
  void remove_prefix(size_t count) {
    size_ -= count;
    if (size_) std::memmove(bytes_.get(), bytes_.get() + count, size_);
  }
};

struct VrsTailScratch {
  VrsTailBuffer buffer;
  bool borrowed = false;
};

// A nested call uses its own storage. There is no shared admission state.
class VrsTailLease {
  VrsTailScratch& scratch_;
  VrsTailBuffer nested_;
  bool primary_;
  static VrsTailScratch& local() {
#ifdef _WIN32
    return VP_TLS(VrsTailScratch);
#else
    thread_local VrsTailScratch scratch;
    return scratch;
#endif
  }
public:
  VrsTailLease() : scratch_(local()), primary_(!scratch_.borrowed) {
    if (primary_) { scratch_.borrowed = true; scratch_.buffer.clear(); }
  }
  ~VrsTailLease() {
    if (primary_) { scratch_.buffer.clear(); scratch_.borrowed = false; }
  }
  VrsTailLease(const VrsTailLease&) = delete;
  VrsTailLease& operator=(const VrsTailLease&) = delete;
  VrsTailBuffer& get() { return primary_ ? scratch_.buffer : nested_; }
};
