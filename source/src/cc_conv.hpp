#pragma once
#include "cc_io.hpp"
#include "vrs.h"
#include <functional>
#include <array>
#include <map>
#include <memory>
#include <thread>

namespace cc {
std::string sha256(std::string_view bytes);
struct Count { uint64_t in = 0, out = 0, emitted = 0; };
struct Counts {
    std::map<std::string, Count> kinds;
    uint64_t unknown = 0;
    void add(const Counts& other);
    Json json() const;
};
struct Converted {
    std::vector<std::string> lines;
    std::string timestamp;
};
Converted conv(std::string_view original, uint64_t offset, Counts* counts = nullptr);
void ordinal(std::string& line, uint64_t n);
struct Position { uint64_t source = 0, ordinal = 0, output = 0; };
struct Tail {
    std::string transcript, rollout, cover, bytes;
    Position before;
    uint64_t observed_end = 0;
    EntryFiles* files = nullptr;
};
struct TailResult {
    uint64_t source_end = 0, output_end = 0, lines = 0, bytes = 0;
    Counts counts;
};
constexpr unsigned cover_slots = 64;
uint64_t cover_max(const std::string& path, EntryFiles* files = nullptr);
Tail prepare_tail(const std::string& transcript, const std::string& rollout, const std::string& cover, EntryFiles* files = nullptr);
TailResult conv_prepared(Tail tail, unsigned slot, vrs_h* lib = nullptr);
TailResult conv_tail(const std::string& transcript, const std::string& rollout,
                     const std::string& cover, unsigned slot, vrs_h* lib = nullptr, EntryFiles* files = nullptr);

// Fixed address space: two bitmap levels, no traversal of a growing file list.
// Capacity exhaustion is an explicit error; live entries are never moved.
template <size_t Capacity> class BitSet {
    static_assert(Capacity % 4096 == 0);
    std::array<std::atomic<uint64_t>, Capacity / 64> low_{};
    std::array<std::atomic<uint64_t>, Capacity / 4096> high_{};
public:
    void add(size_t i) {
        low_[i / 64].fetch_or(uint64_t(1) << (i % 64), std::memory_order_seq_cst);
        high_[i / 4096].fetch_or(uint64_t(1) << ((i / 64) % 64), std::memory_order_seq_cst);
    }
    bool take(size_t& i, size_t seed = 0) {
        for (size_t k = 0; k < high_.size(); ++k) {
            size_t h = (k + seed) % high_.size(); uint64_t groups = high_[h].exchange(0, std::memory_order_seq_cst);
            if (!groups) continue;
            size_t g = size_t(__builtin_ctzll(groups)); groups &= groups - 1;
            if (groups) high_[h].fetch_or(groups, std::memory_order_seq_cst);
            size_t l = h * 64 + g; uint64_t bits = low_[l].exchange(0, std::memory_order_seq_cst);
            if (!bits) continue;
            size_t bit = size_t(__builtin_ctzll(bits)); bits &= bits - 1;
            if (bits) { low_[l].fetch_or(bits, std::memory_order_seq_cst); high_[h].fetch_or(uint64_t(1) << g, std::memory_order_seq_cst); }
            i = l * 64 + bit; return true;
        }
        return false;
    }
};
class Resident {
    struct Job { std::function<void(unsigned)> fn; };
    struct ReadyJob { std::function<bool()> ready; std::function<void(unsigned)> fn; };
    std::array<std::atomic<Job*>, 4096> jobs_{};
    std::array<std::atomic<ReadyJob*>, 4096> ready_jobs_{};
    std::array<std::atomic<bool>, 4096> ready_reserved_{};
    BitSet<4096> work_;
    BitSet<4096> ready_work_;
    BitSet<65536> files_;
    std::function<void(size_t, unsigned)> file_fn_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> signal_{0};
    std::atomic<uint64_t> active_{0};
    bool one(unsigned wi);
public:
    Resident(unsigned workers, std::function<void(size_t, unsigned)> file_fn = {});
    ~Resident();
    void stop_and_join();  // shutdown only; owner remains visible to callbacks
    void submit(std::function<void(unsigned)> fn, unsigned submitter_slot);
    static constexpr size_t no_ready_slot = 4096;
    size_t prepare_ready(std::function<bool()> ready, std::function<void(unsigned)> fn);
    void notify_ready(size_t id);
    void mark(size_t id);
    void drain();  // testing/shutdown only; production chunks never wait here
};
void conv_parallel(Tail tail, Resident& resident, unsigned submitter_slot, vrs_h* lib,
                   std::function<void(TailResult, std::string)> finished, size_t chunk_bytes = 2 * 1024 * 1024);
}
