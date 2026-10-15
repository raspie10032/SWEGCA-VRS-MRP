#ifdef _WIN32
#include "../tools/longpath_probe.hpp"
#endif
#include "cc_conv.hpp"
#include "cc_markers.hpp"
#include "vrs_witness.h"
#include "cc_mcp.hpp"
#include "cc_page.hpp"
#include "cx_mode.hpp"
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <unordered_map>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace cc {
// startup: the file is in the first scan's set F0 and its tail has not been closed since it was marked (KX1, L2 B').
struct Entry {
    std::string transcript, rollout;
    std::atomic<bool> startup{false}, dirty{false};
    std::atomic<uint64_t> enumerated_size{UINT64_MAX};
    EntryFiles handles;
};
class FileTable {
    std::array<std::atomic<Entry*>, 65536> slots_{};
public:
    ~FileTable() { for (auto& slot : slots_) delete slot.load(); }
    size_t add(std::string path, std::string rollout) {
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : path) { hash ^= c; hash *= 1099511628211ull; }
        std::unique_ptr<Entry> candidate;
        for (size_t n = 0; n < slots_.size(); ++n) {
            size_t i = (hash + n) % slots_.size(); Entry* p = slots_[i].load(std::memory_order_acquire);
            if (!p) {
                if (!candidate) candidate.reset(new Entry{path, rollout});
                if (slots_[i].compare_exchange_strong(p, candidate.get(), std::memory_order_acq_rel)) { candidate.release(); return i; }
            }
            if (p->transcript == path) return i;
        }
        throw std::runtime_error("FILE_TABLE_FULL");
    }
    Entry& get(size_t i) { Entry* p = slots_.at(i).load(std::memory_order_acquire); if (!p) throw std::runtime_error("FILE_ID_UNKNOWN"); return *p; }
};
struct Options {
    std::string projects, data, endpoint, change_markers, writer_markers;
    std::vector<std::string> read_roots;
    unsigned workers = 6, connections = 16;
    uint64_t timeout_ms = 5000;
    bool test_metrics = std::getenv("VRS_CC_TEST_METRICS") != nullptr;
    // VRS_onePC_v1.md B6–B8: the app root, the old data a bridge asks to move here, the event a bridge waits on
    std::string codex_home, migrate_claude, migrate_codex;
    uintptr_t ready_event = 0;
};

class Daemon {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point began_ = Clock::now();
    long long elapsed_us() const { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began_).count(); }
    Options opts_;
    std::atomic<uint64_t> cover_reads_{0}, cover_us_{0}, skipped_{0}, overflows_{0};
    std::atomic<bool> open_started_{false}, restore_seen_{false};
    std::atomic<bool> test_drop_{false};
    bool test_markers_only_ = false;
    FileTable files_;
    std::atomic<vrs_h*> lib_{nullptr};
    // KX1: pending is the cardinality of the unfinished F0 facts plus the scan fact. Arrival order has no semantic meaning.
    std::atomic<int64_t> pending_{1};
    size_t open_check_ = Resident::no_ready_slot;
    std::atomic<bool> scanning_{true};
    std::string sessions_, idx_;
    std::atomic<int> open_rc_{0};         // the vrs_open failure, named in every answer (B1: -5 roots overlap)
    std::shared_ptr<CxCore> cx_;          // VRS_onePC_v1.md B6: the app share (null: no app root on this PC)
    std::string cx_sessions_, cx_marks_, cx_writers_;
    vrs_h* lib() const { return lib_.load(std::memory_order_acquire); }
    // KL1 (doc 14.13): ready = lib opened and its open-time restore finished (vrs_ready == 1).
    // Before that a request is answered at once, not held in vrs_wait; ready never returns to 0.
    vrs_h* ready_lib() const {
        vrs_h* h = lib(); if (!h) throw std::runtime_error("NOT_READY");
        const int rd = vrs_ready(h);
        if (rd < 0) throw std::runtime_error("VRS_READY_" + std::to_string(rd));
        if (rd != 1) throw std::runtime_error("NOT_READY");
        return h;
    }
    void open_lib() {
        if (stopping_.load()) return;
        const auto began = Clock::now();
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_PRELIB elapsed_us=%lld cover_reads=%llu cover_sum_us=%llu skipped=%llu\n",
            elapsed_us(), (unsigned long long)cover_reads_.load(), (unsigned long long)cover_us_.load(), (unsigned long long)skipped_.load());
        vrs_opts vo{}; vo.index_dir = idx_.c_str(); vo.workers = opts_.workers;
        // VRS_onePC_v1.md B6: the Claude root (tag "", the host recovery as before) and, when this PC has one, the app root
        vrs_root roots[2]{};
        roots[0].tag = ""; roots[0].dir = sessions_.c_str(); roots[0].recover_context = this;
        roots[0].recover = [](void* context) {
            auto& d = *static_cast<Daemon*>(context);
            if (!d.stopping_.load()) d.recover_watch("lib");
        };
        if (cx_) {
            roots[1].tag = "_cx/"; roots[1].dir = cx_sessions_.c_str();
            roots[1].change_markers = cx_marks_.c_str(); roots[1].writer_markers = cx_writers_.c_str();
        }
        vo.roots = roots; vo.nroots = cx_ ? 2 : 1;
#ifndef _WIN32
        static unsigned cpus[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        vo.cpus = cpus; vo.ncpus = 12;
#endif
        vrs_h* h = nullptr; int rc = vrs_open(&vo, &h);
        if (rc) { std::fprintf(stderr, "vrs_open: %d\n", rc); open_rc_.store(rc); watch_error_.store(5); return; }
        lib_.store(h, std::memory_order_release);
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_OPEN us=%lld elapsed_us=%lld\n", (long long)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count(), elapsed_us());
    }
#ifdef _WIN32
    // VRS_onePC_v1.md B8: once, before the home has an index: the old data folders a bridge named are renamed into the home (never
    // copied: a rename that fails leaves that share to be built here). The originals are not touched. migrated.txt undoes it.
    void migrate() {
        auto is_dir = [](const std::string& p) { std::error_code ec; return fs::is_directory(vp_io_path(p), ec); };
        auto exists = [](const std::string& p) { std::error_code ec; return fs::exists(vp_io_path(p), ec); };
        if (exists(opts_.data + "/idx")) return;
        std::vector<std::pair<std::string, std::string>> plan;
        if (!opts_.migrate_claude.empty() && is_dir(opts_.migrate_claude) && canonical(opts_.migrate_claude) != opts_.data)
            for (const char* n : {"cc_rollout", "idx", "cover", "change-markers", "writer-markers"})
                plan.push_back({canonical(opts_.migrate_claude) + "/" + n, opts_.data + "/" + n});
        // the app's shards go below the Claude index (tag "_cx/"), so they come after it
        if (!opts_.migrate_codex.empty() && is_dir(opts_.migrate_codex)) {
            plan.push_back({canonical(opts_.migrate_codex) + "/idx", opts_.data + "/idx/_cx"});
            plan.push_back({canonical(opts_.migrate_codex) + "/cstar", opts_.data + "/cstar"});
        }
        std::string done;
        for (const auto& [from, to] : plan) {
            if (!is_dir(from) || exists(to)) continue;
            { std::error_code ec; fs::create_directories(vp_io_path(to).parent_path(), ec); }
            if (MoveFileExW(vp_io_path(from).c_str(), vp_io_path(to).c_str(), 0)) done += from + "\t" + to + "\n";
            else std::fprintf(stderr, "vrs_ccd: MIGRATE_RENAME_FAILED_%lu %s\n", (unsigned long)GetLastError(), from.c_str());
        }
        if (!done.empty()) { std::ofstream out(vp_io_path(opts_.data + "/migrated.txt"), std::ios::binary | std::ios::app); out << done; }
    }
#endif
    void finish_one() {
        pending_.fetch_sub(1, std::memory_order_acq_rel);
        // Notification never executes open_lib, including under work-set saturation.
        resident_->notify_ready(open_check_);
    }
    void settle(Entry& e) { if (e.startup.exchange(false, std::memory_order_acq_rel)) finish_one(); }
    void end_scan() { if (scanning_.exchange(false, std::memory_order_acq_rel)) finish_one(); }
    std::unique_ptr<Resident> resident_;
    std::thread watcher_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> watch_error_{0};
    std::atomic<uint32_t> watch_ready_{0};
    int stop_fd_ = -1;
    std::vector<std::thread> requests_;
#ifdef _WIN32
    HANDLE first_pipe_ = INVALID_HANDLE_VALUE;
    std::array<std::atomic<HANDLE>, 64> pipe_threads_{};
    std::array<std::atomic<bool>, 64> pipe_finished_{};
#else
    int listen_fd_ = -1;
#endif
    Entry& identify(const std::string& path) {
        std::string t = canonical(path);
        if (!under(t, opts_.projects) || vp_path(t).extension() != ".jsonl") throw std::runtime_error("TRANSCRIPT_OUTSIDE_ROOT");
        const std::string rel = t.substr(opts_.projects.size() + 1);
        return files_.get(files_.add(t, opts_.data + "/cc_rollout/" + rel));
    }
    void mark(const std::string& path, bool initial = false, uint64_t size = UINT64_MAX) {
        if (vp_path(path).extension() != ".jsonl") return;
        const auto t = size == UINT64_MAX ? canonical(path) : path; if (!under(t, opts_.projects)) throw std::runtime_error("TRANSCRIPT_OUTSIDE_ROOT");
        const size_t id = files_.add(t, opts_.data + "/cc_rollout/" + t.substr(opts_.projects.size() + 1));
        Entry& entry = files_.get(id);
        if (size == UINT64_MAX) entry.dirty.store(true, std::memory_order_release);
        else entry.enumerated_size.store(size, std::memory_order_release);
        // the ticket stands before the mark, so the work after the mark closes the tail it counts for (T-kx1)
        if (initial && !files_.get(id).startup.exchange(true, std::memory_order_acq_rel)) pending_.fetch_add(1, std::memory_order_acq_rel);
        resident_->mark(id);
    }
    std::string cover(const Entry& e) const {
        return opts_.data + "/cover/" + e.transcript.substr(opts_.projects.size() + 1) + ".cv";
    }
    void clean_marker(Entry& e) {
        if (opts_.change_markers.empty()) return;
        const auto name = marker_name(opts_.change_markers, e.transcript);
        if (!vp_exists(name.c_str())) return;
        auto source_lease = e.handles.source.get(e.transcript, false);
        File& source = *source_lease;
        const uint64_t c = cover_max(cover(e), &e.handles);
        source.check_source(e.transcript);
        if (c < source.size()) return; // Keep incomplete lines and failed tails.
        marker_remove(name);
        // Mandatory even when a peer already unlinked the name.
        if (source.size() > c) {
            marker_publish(opts_.change_markers, e.transcript);
            mark(e.transcript);
        }
    }
    void mark_file(const std::string& name) {
        if (vp_path(name).extension() != ".mark") return;
        const auto source = marker_read(name);
        if (!source.empty()) mark(source);
    }
    void scan_markers() {
        const auto began = Clock::now(); uint64_t visited = 0;
        for (vp_directory_iterator i(vp_io_path(opts_.change_markers)), end; i != end; ++i) {
            ++visited;
            std::error_code ec; const auto status = i->symlink_status(ec);
            if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) continue;
            if (ec) throw std::runtime_error("MARKER_STATUS_FAILED");
            if (fs::is_symlink(status) || !fs::is_regular_file(status)) throw std::runtime_error("MARKER_ENTRY_INVALID");
            mark_file(vp_u8(i->path()));
        }
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_MARKERS entries=%llu us=%lld\n",
            (unsigned long long)visited, (long long)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count());
    }
    struct Visits { uint64_t marks = 0, writers = 0, sources = 0; };
    void writer_file(const std::string& name, Visits& visits) {
        if (vp_path(name).extension() != ".mark") return;
        const auto w = vw::read(name); if (w.source.empty()) return;
        if (!under(w.source, opts_.projects)) throw std::runtime_error("WRITER_OUTSIDE_ROOT");
        bool live = true;
        try { live = vw::alive(w); }
        catch (const std::exception& e) { std::fprintf(stderr, "writer retained: %.512s\n", e.what()); }
        bool complete = true;
        visits.sources += vw::sources(w, [&](const std::string& source) {
            Entry& e = identify(source);
            File original(e.transcript);
            original.check_source(e.transcript);
            if (cover_max(cover(e), &e.handles) < original.size()) complete = false;
            mark(source);
        });
        if (live || !complete) return;
        marker_remove(name); // Only the daemon retires W, and only a dead identity.
        // Every remover performs the G2-T2 post-unlink size check for all members.
        vw::sources(w, [&](const std::string& source) {
            Entry& e = identify(source); File original(e.transcript);
            original.check_source(e.transcript);
            if (cover_max(cover(e), &e.handles) < original.size()) {
                marker_publish(opts_.change_markers, e.transcript); mark(e.transcript);
            }
        });
    }
    void scan_writers(Visits& visits) {
        visits.writers += vw::scan(opts_.writer_markers, true, [&](const vw::Witness& w) { writer_file(w.name, visits); });
    }
    void witness_event(const std::string& path) {
        if (under(path, opts_.writer_markers)) { Visits v; writer_file(path, v); }
        else if (under(path, opts_.change_markers)) mark_file(path);
    }
    void refresh(Entry& e) {
        if (auto* h = lib()) if (vrs_refresh(h, e.rollout.c_str())) throw std::runtime_error("LIB_REFRESH_FAILED");
    }
    void work(size_t id, unsigned slot) {
        Entry* entry = nullptr;
        try {
            Entry& e = files_.get(id); entry = &e;
            const bool dirty = e.dirty.exchange(false, std::memory_order_acq_rel);
            uint64_t size = e.enumerated_size.load(std::memory_order_acquire);
            if (dirty || size == UINT64_MAX) {
                auto source_lease = e.handles.source.get(e.transcript, false);
                File& source = *source_lease;
                source.check_source(e.transcript); size = source.size();
            }
            const auto cover_begin = Clock::now();
            const uint64_t covered = cover_max(cover(e), &e.handles);
            if (opts_.test_metrics) {
                cover_reads_.fetch_add(1, std::memory_order_relaxed);
                cover_us_.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - cover_begin).count(), std::memory_order_relaxed);
            }
            if (covered > size) {
                // An enumeration may predate another worker's completed append.
                auto source_lease = e.handles.source.get(e.transcript, false);
                File& source = *source_lease;
                source.check_source(e.transcript); size = source.size();
                if (covered > size) throw std::runtime_error("SOURCE_TRUNCATED");
            }
            if (covered == size) { skipped_.fetch_add(1, std::memory_order_relaxed); refresh(e); clean_marker(e); settle(e); return; }
            Tail t = prepare_tail(e.transcript, e.rollout, cover(e), &e.handles);
            if (t.bytes.size() > 8 * 1024 * 1024) {
                conv_parallel(std::move(t), *resident_, slot, lib(), [this, entry](TailResult, std::string error) {
                    if (!error.empty()) std::fprintf(stderr, "conversion: %.512s\n", error.c_str());
                    if (error.empty()) try { refresh(*entry); clean_marker(*entry); }
                        catch (const std::exception& e) { std::fprintf(stderr, "conversion: %.512s\n", e.what()); }
                    settle(*entry);
                });
            } else { conv_prepared(std::move(t), slot, lib()); refresh(e); clean_marker(e); settle(e); }
        } catch (const std::exception& x) {
            std::fprintf(stderr, "conversion: %.512s\n", x.what());
            if (entry) settle(*entry);  // a failed tail still lowers its ticket, so lib7 opens
        }
    }
    template <class Add> void scan(const std::string& directory, Add&& add_watch, bool initial = false) {
        if (stopping_.load()) return;
        add_watch(directory);
#ifdef _WIN32
        WIN32_FIND_DATAW data{};
        HANDLE h = FindFirstFileExW(vp_io_path(directory + "/*").c_str(), FindExInfoBasic, &data,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
            throw std::runtime_error("DIRECTORY_ENUM_FAILED");
        }
        struct FindCloseGuard { HANDLE h; ~FindCloseGuard() { FindClose(h); } } close{h};
        do {
            if (stopping_.load()) return;
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..") || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            const auto path = directory + "/" + vp_u8(fs::path(data.cFileName));
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scan(path, add_watch, initial);
            else mark(path, initial, (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
        } while (FindNextFileW(h, &data));
        if (GetLastError() != ERROR_NO_MORE_FILES) throw std::runtime_error("DIRECTORY_ENUM_FAILED");
#else
        for (vp_directory_iterator i(vp_io_path(directory)), end; i != end; ++i) {
            if (stopping_.load()) return;
            std::error_code ec; auto status = i->symlink_status(ec);
            if (ec || fs::is_symlink(status)) continue;
            const auto path = vp_u8(i->path());
            if (fs::is_directory(status)) scan(path, add_watch, initial);
            else if (fs::is_regular_file(status)) { auto n = i->file_size(ec); if (!ec) mark(path, initial, n); }
        }
#endif
    }
    Visits recover_watch(const char* origin = "daemon") {
        if (stopping_.load()) return {};
        const uint64_t n = overflows_.fetch_add(1) + 1;
        const auto start = Clock::now();
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_OVERFLOW count=%llu\n", (unsigned long long)n);
        Visits v;
        v.marks = vw::scan(opts_.change_markers, false, [&](const vw::Witness& w) { ++v.sources; mark(w.source); });
        scan_writers(v);
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_WITNESSES origin=%s marks=%llu writers=%llu sources=%llu\n",
            origin, (unsigned long long)v.marks, (unsigned long long)v.writers, (unsigned long long)v.sources);
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_MARKERS entries=%llu us=%lld\n", (unsigned long long)v.marks,
            (long long)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_RESCAN count=%llu us=%lld\n", (unsigned long long)n,
            (long long)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        return v;
    }
    void initial_done(Clock::time_point start) {
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_SCAN us=%lld elapsed_us=%lld\n",
            (long long)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count(), elapsed_us());
        end_scan(); wake_word(watch_ready_);
    }
#ifdef _WIN32
    void watch_windows(Clock::time_point scan_begin) {
        struct Watch {
            std::string root;
            HANDLE dir = INVALID_HANDLE_VALUE, event = nullptr;
            OVERLAPPED ov{}; bool pending = false;
            std::vector<DWORD> storage;
            ~Watch() {
                if (pending) { DWORD n; CancelIoEx(dir, &ov); GetOverlappedResult(dir, &ov, &n, TRUE); }
                if (event) CloseHandle(event);
                if (dir != INVALID_HANDLE_VALUE) CloseHandle(dir);
            }
        } watches[3];
        watches[0].root = opts_.projects;
        watches[1].root = opts_.change_markers;
        watches[2].root = opts_.writer_markers;
        DWORD bytes = 65536;
        if (opts_.test_metrics) if (const char* v = std::getenv("VRS_CC_TEST_WATCH_BYTES")) bytes = DWORD(std::clamp<unsigned long>(std::stoul(v), 64, 65536)) & ~DWORD(3);
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        auto arm = [&](Watch& w) {
            w.ov = {}; w.ov.hEvent = w.event; ResetEvent(w.event);
            if (!ReadDirectoryChangesW(w.dir, w.storage.data(), bytes, TRUE, filter, nullptr, &w.ov, nullptr))
                throw std::runtime_error("WATCH_ARM_FAILED");
            w.pending = true;
        };
        HANDLE waits[4]{reinterpret_cast<HANDLE>(intptr_t(stop_fd_))};
        for (size_t i = 0; i < 3; ++i) {
            auto& w = watches[i];
            w.dir = CreateFileW(vp_io_path(w.root).c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (w.dir == INVALID_HANDLE_VALUE || !w.event) throw std::runtime_error("WATCH_OPEN_FAILED");
            w.storage.resize(bytes / sizeof(DWORD)); waits[i + 1] = w.event; arm(w);
        }
        scan(opts_.projects, [](const std::string&) {}, true); initial_done(scan_begin);
        scan_markers(); { Visits v; scan_writers(v); }
        if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_WATCH_READY bytes=%lu\n", (unsigned long)bytes);
        auto next_check = Clock::now() + std::chrono::seconds(1);
        while (!stopping_.load()) {
            if (Clock::now() >= next_check) {
                if (!test_drop_.load()) { Visits v; scan_writers(v); }
                next_check = Clock::now() + std::chrono::seconds(1);
            }
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(next_check - Clock::now()).count();
            DWORD ready = WaitForMultipleObjects(4, waits, FALSE, DWORD(std::max<long long>(0, left)));
            if (ready == WAIT_OBJECT_0) break;
            if (ready == WAIT_TIMEOUT) continue;
            if (ready < WAIT_OBJECT_0 + 1 || ready >= WAIT_OBJECT_0 + 4) throw std::runtime_error("WATCH_WAIT_FAILED");
            auto& w = watches[ready - WAIT_OBJECT_0 - 1];
            DWORD n = 0; BOOL ok = GetOverlappedResult(w.dir, &w.ov, &n, FALSE); w.pending = false;
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && error != ERROR_NOTIFY_ENUM_DIR) throw std::runtime_error("WATCH_READ_FAILED");
            std::vector<DWORD> completed((n + 3) / 4);
            if (n) std::memcpy(completed.data(), w.storage.data(), n);
            arm(w); // Observation is armed before either recovery or path processing.
            if (test_drop_.load() || (test_markers_only_ && w.root == opts_.projects)) continue;
            if (!ok || !n) { recover_watch(); continue; }
            for (DWORD at = 0; at < n;) {
                constexpr DWORD head = offsetof(FILE_NOTIFY_INFORMATION, FileName);
                if (n - at < head) throw std::runtime_error("WATCH_RECORD_INVALID");
                const auto* f = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const char*>(completed.data()) + at);
                if (f->FileNameLength > n - at - head || (f->FileNameLength & 1)) throw std::runtime_error("WATCH_RECORD_INVALID");
                if (f->Action != FILE_ACTION_REMOVED && f->Action != FILE_ACTION_RENAMED_OLD_NAME) {
                    auto rel = vp_u8(fs::path(std::wstring(f->FileName, f->FileNameLength / sizeof(WCHAR))));
                    std::replace(rel.begin(), rel.end(), '\\', '/');
                    const auto path = w.root + "/" + rel;
                    DWORD attr = GetFileAttributesW(vp_io_path(path).c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        if (w.root != opts_.projects) witness_event(path);
                        else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) mark(path);
                    }
                }
                if (!f->NextEntryOffset) break;
                if (f->NextEntryOffset < head || f->NextEntryOffset > n - at) throw std::runtime_error("WATCH_RECORD_INVALID");
                at += f->NextEntryOffset;
            }
        }
    }
#endif
    void watch() {
        const auto scan_begin = std::chrono::steady_clock::now();
        try {
#ifdef _WIN32
            watch_windows(scan_begin);
#else
            const int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK); if (fd < 0) throw std::runtime_error("INOTIFY_OPEN_FAILED");
            struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
            std::unordered_map<int, std::string> dirs;
            auto add = [&](const std::string& path) {
                int wd = inotify_add_watch(fd, path.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
                if (wd < 0) throw std::runtime_error("INOTIFY_WATCH_FAILED"); dirs[wd] = path;
            };
            add(opts_.change_markers); add(opts_.writer_markers);
            if (test_markers_only_) scan(opts_.projects, [](const std::string&) {}, true);
            else scan(opts_.projects, add, true);
            scan_markers(); { Visits v; scan_writers(v); }
            initial_done(scan_begin);
            if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_WATCH_READY bytes=65536\n");
            alignas(inotify_event) char buf[65536]; pollfd fds[2]{{fd, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
            auto next_check = Clock::now() + std::chrono::seconds(1);
            while (!stopping_.load()) {
                if (Clock::now() >= next_check) {
                    if (!test_drop_.load()) { Visits v; scan_writers(v); }
                    next_check = Clock::now() + std::chrono::seconds(1);
                }
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(next_check - Clock::now()).count();
                int rc = poll(fds, 2, int(std::max<long long>(0, left))); if (rc < 0 && errno == EINTR) continue; if (rc < 0) throw std::runtime_error("WATCH_POLL_FAILED");
                if (fds[1].revents) break;
                if (!rc) continue;
                ssize_t n = ::read(fd, buf, sizeof buf); if (n < 0 && errno == EAGAIN) continue;
                if (n < 0) throw std::runtime_error("WATCH_READ_FAILED");
                for (ssize_t at = 0; at < n;) {
                    const auto* ev = reinterpret_cast<const inotify_event*>(buf + at); at += sizeof(*ev) + ev->len;
                    if (test_drop_.load()) continue;
                    if (ev->mask & IN_Q_OVERFLOW) { recover_watch(); continue; }
                    auto it = dirs.find(ev->wd); if (it == dirs.end()) continue;
                    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) { watch_error_.store(3); continue; }
                    if (!ev->len) continue; const auto path = it->second + "/" + ev->name;
                    if (it->second == opts_.change_markers || it->second == opts_.writer_markers) {
                        if (vp_exists(path.c_str())) witness_event(path);
                    } else if (ev->mask & IN_ISDIR) {
                        if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && vp_exists(path.c_str())) add(path);
                    } else if (vp_exists(path.c_str()) && !fs::is_symlink(vp_io_path(path))) mark(path);
                }
            }
#endif
        } catch (const std::exception& e) { watch_error_.store(1); end_scan(); wake_word(watch_ready_); std::fprintf(stderr, "watch: %.512s\n", e.what()); }
    }
    Json assembly(Entry& e, const TailResult& tail) {
        if (!tail.output_end) { Json j = Json::obj(); j.set("items", Json::arr()); return j; }
        vrs_h* lib_ = ready_lib();
        int rc = vrs_wait(lib_,e.rollout.c_str(), opts_.timeout_ms);
        if (rc) throw std::runtime_error("VRS_WAIT_" + std::to_string(rc));
        rc = vrs_wait_off(lib_, e.rollout.c_str(), tail.output_end, opts_.timeout_ms);
        if (rc == -2) {
            // A peer may have completed the bytes immediately before this call
            // captured C(T), while its inserts have not begun. lib7's first
            // assembly discovers that existing file through its normal API.
            char* raw = nullptr; size_t n = 0;
            int discovered = vrs_assemble_b(lib_, e.rollout.c_str(), 258400, &raw, &n); vrs_free(raw);
            if (discovered < 0 || discovered == 2) throw std::runtime_error("VRS_ASSEMBLE_" + std::to_string(discovered));
            rc = vrs_wait_off(lib_, e.rollout.c_str(), tail.output_end, opts_.timeout_ms);
        }
        if (rc) throw std::runtime_error("VRS_WAIT_OFF_" + std::to_string(rc));
        char* raw = nullptr; size_t n = 0;
        rc = vrs_assemble_b(lib_, e.rollout.c_str(), 258400, &raw, &n);
        std::unique_ptr<char, decltype(&vrs_free)> bytes(raw, vrs_free);
        if (rc) throw std::runtime_error("VRS_ASSEMBLE_" + std::to_string(rc));
        return parse(std::string_view(raw, n));
    }
    Restored restore_item(const File& original, const std::string& filename, const Json& payload) {
        const auto& c = payload.at("cc"); const uint64_t o = c.at("o").uint(), n = c.at("n").uint();
        const std::string k = c.at("k").text(), hash = c.at("h").text();
        const std::string raw = original.range(o, n);
        if (sha256(raw) != hash) throw std::runtime_error("SOURCE_SHA_MISMATCH_AT_" + std::to_string(o));
        const Json j = parse(raw); const std::string role = payload.at("role").text();
        Key key = key_of(std::to_string(o) + ":" + k); std::string body;
        {
            // KX2a: a queued_command prompt is read by the same block rule as message content.
            const bool queued = j.get("type") == "attachment";
            if (queued && (role != "user" || j.at("attachment").get("type") != "queued_command" ||
                j.at("attachment").at("origin").get("kind") != "human")) throw std::runtime_error("SOURCE_ROLE_MISMATCH");
            if (!queued && j.get("type") != role) throw std::runtime_error("SOURCE_ROLE_MISMATCH");
            const Json& content = queued ? j.at("attachment").at("prompt") : j.at("message").at("content");
            if (content.kind == Json::string && key.k == 0) body = content.scalar;
            else if (content.kind == Json::array && key.k < content.values.size()) {
                const Json& b = content.values[size_t(key.k)];
                if (b.get("type") == "text") body = b.at("text").text();
                else if (b.get("type") == "image" && role == "user")
                    body = "[image " + b.at("source").at("media_type").text() + " · " + filename + "#" + std::to_string(o) + "+" + std::to_string(n) + " k=" + k + "]";
                else throw std::runtime_error("SOURCE_BLOCK_MISMATCH");
            } else throw std::runtime_error("SOURCE_BLOCK_RANGE");
        }
        const std::string ts = j.at("timestamp").text();
        std::string header = "[" + ts + " · " + (role == "user" ? "사용자" : "답") + " · " + filename + "#" + std::to_string(o) + "+" + std::to_string(n) + " k=" + k + " · sha " + hash.substr(0, 16) + "]\n";
        Json item = Json::obj(); item.set("cc", c).set("role", Json::str(role)).set("timestamp", Json::str(ts)).set("text", Json::str(body));
        size_t units = utf16_units(body); return {key, std::move(item), std::move(header), units};
    }
    Json restore(const Json& request, unsigned slot, bool diagnostic) {
        Entry& e = identify(request.at("transcript").text());
        const size_t budget = diagnostic ? 95000 : budget_of(request);
        ready_lib();  // KX1/KL1: before lib7 is ready, a request is answered at once, not held
        const TailResult tail = conv_tail(e.transcript, e.rollout, cover(e), slot, lib(), &e.handles);
        clean_marker(e);
        auto source_lease = e.handles.source.get(e.transcript, false);
        File& source = *source_lease; uint64_t boundary = UINT64_MAX;
        if (!diagnostic) {
            ReverseLines lines(source, tail.source_end); std::string line; uint64_t off;
            while (lines.previous(line, off)) {
                Json j = parse(line);
                if (j.get("type") == "system" && j.get("subtype") == "compact_boundary") { boundary = off; break; }
            }
        }
        // Even an empty restoration has closed its caller's new tail first.
        Json a = assembly(e, tail);
        if (diagnostic) {
            Json users = Json::arr();
            for (const auto& p : a.at("items").values) if (p.get("type") == "message" && p.get("role") == "user") users.values.push_back(p.at("cc"));
            Json out = Json::obj(); out.set("ok", Json::flag(true)).set("users", std::move(users)); return out;
        }
        const std::string filename = vp_u8(vp_path(e.transcript).filename()), session = vp_u8(vp_path(e.transcript).stem());
        std::vector<Restored> items;
        if (boundary != UINT64_MAX) for (const auto& p : a.at("items").values) {
            if (p.get("type") != "message" || (p.get("role") != "user" && p.get("role") != "assistant")) continue;
            if (p.at("cc").at("o").uint() < boundary) items.push_back(restore_item(source, filename, p));
        }
        return restore_page(items, request, budget, session, boundary);
    }
    Json read(const Json& request) {
        const size_t budget = budget_of(request); const std::string path = canonical(request.at("path").text());
        bool permitted = under(path, opts_.projects) || (cx_ && under(path, cx_sessions_));  // B6: either app's originals
        for (const auto& root : opts_.read_roots) permitted = permitted || under(path, root);
        if (!permitted) throw std::runtime_error("READ_OUTSIDE_ROOT");
        (void)budget; return read_page(path, request);
    }
public:
    explicit Daemon(Options opts) : opts_(std::move(opts)) {}
    std::string respond(std::string_view bytes, unsigned slot) {
        try {
            if (open_rc_.load()) throw std::runtime_error("VRS_OPEN_" + std::to_string(open_rc_.load()));
            if (watch_error_.load()) throw std::runtime_error("WATCH_INCOMPLETE_" + std::to_string(watch_error_.load()));
            Json j = parse(bytes); const std::string op = j.at("op").text();
            // VRS_onePC_v1.md B6: proto 2 = one daemon, two shares; a request that names a thread is the app's
            if (op == "hello") {
                Json roots = Json::arr(); roots.values.push_back(Json::str(""));
                if (cx_) roots.values.push_back(Json::str("_cx/"));
                Json out = Json::obj(); out.set("ok", Json::flag(true)).set("proto", Json::num(2)).set("roots", std::move(roots));
                return out.dump() + '\n';
            }
            if (j.find("thread") && op != "read") {
                if (!cx_) throw std::runtime_error("CODEX_ROOT_ABSENT");
                return cx_core_respond(*cx_, bytes);
            }
            if (opts_.test_metrics && op == "test_watch") {
                return std::string("{\"ok\":true,\"drop\":") + (test_drop_.load() ? "true" : "false") + "}\n";
            }
            if (opts_.test_metrics && op == "test_overflow") {
                if (j.get("origin") == "lib") {
                    if (vrs_dbg_overflow(lib())) throw std::runtime_error("TEST_LIB_OVERFLOW_FAILED");
                    return "{\"ok\":true}\n";
                }
                auto v = recover_watch("test");
                return "{\"ok\":true,\"marks\":" + std::to_string(v.marks) + ",\"writers\":" + std::to_string(v.writers) + ",\"sources\":" + std::to_string(v.sources) + "}\n";
            }
            if (op == "restore") {
                const auto began = std::chrono::steady_clock::now();
                std::string reply = restore(j, slot, false).dump() + '\n';
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
                if (opts_.test_metrics) {
                    const bool first = !restore_seen_.exchange(true);
                    if (first) std::fprintf(stderr, "VRS_TEST_FIRST_RESTORE elapsed_us=%lld\n", elapsed_us());
                    vrs_dbg d{}; if (lib()) vrs_dbg_stats(lib(), &d);
                    std::fprintf(stderr, "VRS_TEST_RESTORE us=%lld ready_us=%lld shards_loaded=%llu shards_rebuilt=%llu\n",
                        (long long)us, (long long)d.ready_us, (unsigned long long)d.shards_loaded, (unsigned long long)d.shards_rebuilt);
                }
                return reply;
            }
            if (op == "diag_assemble") return restore(j, slot, true).dump() + '\n';
            if (op == "read") return read(j).dump() + '\n';
            throw std::runtime_error("UNKNOWN_OPERATION");
        } catch (const std::exception& e) {
            std::string message(e.what());
            if (message.size() > 1024) {
                size_t n = 1024; while (n && (uint8_t(message[n]) & 0xc0) == 0x80) --n; message.resize(n);
            }
            Json out = Json::obj(); out.set("ok", Json::flag(false)).set("error", Json::str(message)); return out.dump() + '\n';
        }
    }
    void start();
    void serve();
    ~Daemon();
    int stop_fd() const { return stop_fd_; }
};

#ifndef _WIN32
static volatile sig_atomic_t signal_fd = -1;
static void interrupted(int) { int saved = errno; uint64_t one = 1; if (signal_fd >= 0) { ssize_t n = ::write(signal_fd, &one, sizeof one); (void)n; } errno = saved; }
#endif
void Daemon::start() {
    const auto began = std::chrono::steady_clock::now();
    test_drop_.store(opts_.test_metrics && std::getenv("VRS_CC_TEST_DROP_WATCH"));
    test_markers_only_ = opts_.test_metrics && std::getenv("VRS_CC_TEST_MARKERS_ONLY");
    fs::create_directories(vp_io_path(opts_.data)); opts_.data = canonical(opts_.data);
    // VRS_onePC_v1.md B7: a PC without Claude Code: an empty projects tree in the home (nothing is written into another app's home)
    if (opts_.projects.empty()) { opts_.projects = opts_.data + "/no-projects"; fs::create_directories(vp_io_path(opts_.projects)); }
    opts_.projects = canonical(opts_.projects);
    stop_fd_ = vp_stop_new(); if (stop_fd_ < 0) throw std::runtime_error("STOP_OPEN_FAILED");
#ifdef _WIN32
    // B8: the pipe comes first, so only the process that serves the name moves old data into the home
    first_pipe_ = CreateNamedPipeW(vp_io_path(opts_.endpoint).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    // The bridge that started this process waits for this one fact: the name is served (by this process or an earlier one).
    if (opts_.ready_event) { HANDLE e = reinterpret_cast<HANDLE>(opts_.ready_event); SetEvent(e); CloseHandle(e); opts_.ready_event = 0; }
    if (first_pipe_ == INVALID_HANDLE_VALUE) throw std::runtime_error("PIPE_ALREADY_RUNNING_OR_UNAVAILABLE");
    migrate();
    if (opts_.test_metrics && std::getenv("VRS_CC_TEST_EXIT_AFTER_MIGRATE")) std::exit(0);  // V5: the moved tree is listed with nothing else written
#else
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (listen_fd_ < 0) throw std::runtime_error("SOCKET_OPEN_FAILED");
    sockaddr_un addr{}; addr.sun_family = AF_UNIX;
    // An abstract Unix name is process-lifetime unique and leaves no stale
    // pathname after a crash. The bridge uses the same @name convention.
    if (opts_.endpoint.empty() || opts_.endpoint[0] != '@' || opts_.endpoint.size() >= sizeof addr.sun_path) throw std::runtime_error("SOCKET_EXPECT_ABSTRACT_NAME");
    std::memcpy(addr.sun_path + 1, opts_.endpoint.data() + 1, opts_.endpoint.size() - 1);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), socklen_t(offsetof(sockaddr_un, sun_path) + opts_.endpoint.size()))) throw std::runtime_error("SOCKET_ALREADY_RUNNING_OR_UNAVAILABLE");
    if (listen(listen_fd_, 128)) throw std::runtime_error("SOCKET_LISTEN_FAILED");
#endif
    if (opts_.change_markers.empty()) opts_.change_markers = opts_.data + "/change-markers";
    if (opts_.writer_markers.empty()) opts_.writer_markers = opts_.data + "/writer-markers";
    for (auto* root : {&opts_.change_markers, &opts_.writer_markers}) {
        fs::create_directories(vp_io_path(*root)); *root = canonical(*root); marker_directories(*root);
        if (*root == opts_.projects || under(*root, opts_.projects)) throw std::runtime_error("MARKERS_MUST_BE_OUTSIDE_PROJECTS");
    }
    if (opts_.change_markers == opts_.writer_markers) throw std::runtime_error("MARKER_ROOTS_MUST_DIFFER");
    for (auto& root : opts_.read_roots) root = canonical(root);
    sessions_ = opts_.data + "/cc_rollout"; idx_ = opts_.data + "/idx";
    fs::create_directories(vp_io_path(sessions_)); fs::create_directories(vp_io_path(idx_));
    // B6: the app share, when this PC has the app's sessions tree (nothing is created in another app's home)
    if (!opts_.codex_home.empty()) {
        std::error_code ec;
        if (fs::is_directory(vp_io_path(opts_.codex_home + "/sessions"), ec)) {
            cx_ = cx_core(opts_.codex_home, opts_.data, opts_.read_roots, opts_.timeout_ms, [this] { return lib(); });
            cx_sessions_ = cx_core_sessions(*cx_);
            cx_marks_ = opts_.data + "/cx-marks"; cx_writers_ = opts_.data + "/cx-writers";
            fs::create_directories(vp_io_path(cx_marks_)); fs::create_directories(vp_io_path(cx_writers_));
        }
    }
    if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_START elapsed_us=%lld\n", elapsed_us());
    resident_.reset(new Resident(opts_.workers, [this](size_t id, unsigned slot) { work(id, slot); }));
    open_check_ = resident_->prepare_ready(
        [this] { return pending_.load(std::memory_order_acquire) == 0; },
        [this](unsigned) { bool no = false; if (open_started_.compare_exchange_strong(no, true)) open_lib(); });
    if (open_check_ == Resident::no_ready_slot) throw std::runtime_error("STARTUP_CHECK_CAPACITY");
    watcher_ = std::thread([this] { watch(); });
    for (unsigned k = 0; k < opts_.connections; ++k) requests_.emplace_back([this, k] {
        const unsigned slot = opts_.workers + k;
#ifdef _WIN32
        HANDLE own_thread = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &own_thread,
                             THREAD_TERMINATE, FALSE, 0)) { watch_error_.store(4); pipe_finished_[k].store(true); return; }
        pipe_threads_[k].store(own_thread);
        HANDLE pipe = k == 0 ? first_pipe_ : INVALID_HANDLE_VALUE;
        auto create = [&] { return CreateNamedPipeW(vp_io_path(opts_.endpoint).c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr); };
        while (!stopping_.load()) {
            if (pipe == INVALID_HANDLE_VALUE) pipe = create();
            if (pipe == INVALID_HANDLE_VALUE) { watch_error_.store(4); break; }
            bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            std::string line; bool complete = false;
            if (connected) while (line.size() < 65536) {
                char buf[4096]; DWORD got = 0;
                if (!ReadFile(pipe, buf, sizeof buf, &got, nullptr) || !got) break;
                line.append(buf, got); if (line.find('\n') != line.npos) { complete = true; break; }
            }
            if (complete) {
                std::string reply = respond(std::string_view(line).substr(0, line.find('\n')), slot);
                for (size_t at = 0; at < reply.size();) { DWORD put = 0; if (!WriteFile(pipe, reply.data() + at, DWORD(std::min<size_t>(reply.size() - at, 65536)), &put, nullptr) || !put) break; at += put; }
                FlushFileBuffers(pipe);
            }
            // Keep an instance alive across replacement even when K=1, so a
            // second process cannot acquire FIRST_PIPE_INSTANCE in the gap.
            HANDLE next = stopping_.load() ? INVALID_HANDLE_VALUE : create();
            DisconnectNamedPipe(pipe); CloseHandle(pipe); pipe = next;
        }
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe_finished_[k].store(true);
#else
        (void)k;
        while (!stopping_.load()) {
            int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) { if (errno == EINTR) continue; break; }
            timeval timeout{5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
            std::string line; bool complete = false;
            while (line.size() < 65536) {
                char buf[4096]; ssize_t got = recv(fd, buf, sizeof buf, 0); if (got < 0 && errno == EINTR) continue; if (got <= 0) break;
                line.append(buf, size_t(got)); if (line.find('\n') != line.npos) { complete = true; break; }
            }
            if (complete) {
                std::string reply = respond(std::string_view(line).substr(0, line.find('\n')), slot);
                for (size_t at = 0; at < reply.size();) {
                    ssize_t put = send(fd, reply.data() + at, reply.size() - at, MSG_NOSIGNAL);
                    if (put < 0 && errno == EINTR) continue; if (put <= 0) break; at += size_t(put);
                }
            }
            ::close(fd);
        }
#endif
    });
}
void Daemon::serve() {
#ifdef _WIN32
    WaitForSingleObject(reinterpret_cast<HANDLE>(intptr_t(stop_fd_)), INFINITE);
#else
    signal_fd = stop_fd_; struct sigaction sa{}; sa.sa_handler = interrupted; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr);
    pollfd fd{stop_fd_, POLLIN, 0}; while (poll(&fd, 1, -1) < 0 && errno == EINTR) {}
#endif
}
Daemon::~Daemon() {
    stopping_.store(true); if (stop_fd_ >= 0) vp_stop_set(stop_fd_);
#ifdef _WIN32
    // std::thread's native handle is not a Win32 HANDLE in every MinGW runtime.
    // Use the duplicated OS handle and repeat cancellation only at shutdown,
    // covering a thread that had not entered its blocking OS call yet.
    for (size_t i = 0; i < requests_.size(); ++i) {
        while (!pipe_finished_[i].load()) {
            HANDLE h = pipe_threads_[i].load(); if (h) CancelSynchronousIo(h);
            Sleep(10);
        }
    }
#else
    signal_fd = -1; if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); }
#endif
    for (auto& t : requests_) if (t.joinable()) t.join();
#ifdef _WIN32
    for (auto& h : pipe_threads_) if (h.load()) CloseHandle(h.load());
    if (requests_.empty() && first_pipe_ != INVALID_HANDLE_VALUE) CloseHandle(first_pipe_);
#endif
    if (watcher_.joinable()) watcher_.join();
    if (resident_) resident_->stop_and_join();
    // A lib recovery callback may have passed stopping_ before shutdown.
    // Preserve Resident until vrs_close has joined that callback's watcher.
    if (lib()) vrs_close(lib());
    resident_.reset();
    if (stop_fd_ >= 0) vp_stop_close(stop_fd_);
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_args) {
    std::vector<std::string> args; args.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_args[i], -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) { std::fprintf(stderr, "vrs_ccd: ARGUMENT_UTF16_INVALID\n"); return 1; }
        std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_args[i], -1, s.data(), n, nullptr, nullptr);
        s.pop_back(); args.push_back(std::move(s));
    }
    std::vector<char*> argv; for (auto& s : args) argv.push_back(s.data());
#else
int main(int argc, char** argv) {
#endif
    try {
#ifdef _WIN32
        if (argc == 3 && std::string(argv[1]) == "--test-longpath-fs") return longpath_probe(argv[2]);
#endif
        if (argc == 7 && std::string(argv[1]) == "--writer-root" && std::string(argv[3]) == "--writer-path" && std::string(argv[5]) == "--writer-pid") {
            cc::fs::create_directories(vp_io_path(argv[2]));
            vw::publish(cc::canonical(argv[2]), vw::owner(argv[4], std::stoull(argv[6]))); return 0;
        }
        // D-P1/D-P2 (doc 14.21): the plugin runs this one executable as its MCP server and as its hooks.
        if (argc == 2 && std::string(argv[1]) == "--mcp") return cc::mcp_main();
        // VRS_MCP_codexapp_v1.md 7.3/7.4: the ChatGPT desktop app (Codex) mode, the MCP server and the daemon it starts.
        if (argc == 4 && std::string(argv[1]) == "--mcp" && std::string(argv[2]) == "--host" && std::string(argv[3]) == "codex") return cc::cx_mcp_main();
        if (argc >= 2 && std::string(argv[1]) == "--codex-daemon") {
            std::vector<std::string> rest; for (int i = 2; i < argc; ++i) rest.push_back(argv[i]);
            return cc::cx_daemon_main(rest);
        }
        if (argc == 3 && std::string(argv[1]) == "--hook") return cc::hook_main(argv[2]);
        if (argc == 5 && std::string(argv[1]) == "--writer-hook" && std::string(argv[3]) == "--host")
            return cc::writer_hook(argv[2], argv[4]);
        if (argc == 5 && std::string(argv[1]) == "--mark-root" && std::string(argv[3]) == "--mark-path") {
            const std::string root = cc::canonical(argv[2]);
            cc::marker_directories(root); cc::marker_publish(root, cc::canonical(argv[4])); return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--mark-hook") {
            std::string input; char buf[4096];
            while (size_t n = std::fread(buf, 1, sizeof buf, stdin)) {
                input.append(buf, n); if (input.size() > 1024 * 1024) throw std::runtime_error("HOOK_INPUT_TOO_LARGE");
            }
            const auto j = cc::parse(input); const auto path = j.at("transcript_path").text();
            const auto root = cc::canonical(argv[2]);
            cc::marker_directories(root); cc::marker_publish(root, cc::canonical(path)); return 0;
        }
        cc::Options o;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i]; if (i + 1 >= argc) throw std::runtime_error("OPTION_VALUE_MISSING"); std::string v = argv[++i];
            if (a == "--projects") o.projects = v; else if (a == "--data") o.data = v;
            else if (a == "--change-markers") o.change_markers = v;
            else if (a == "--writer-markers") o.writer_markers = v;
            else if (a == "--endpoint") o.endpoint = v; else if (a == "--read-root") o.read_roots.push_back(v);
            else if (a == "--workers") o.workers = unsigned(std::stoul(v));
            else if (a == "--connections") o.connections = unsigned(std::stoul(v));
            else if (a == "--timeout-ms") o.timeout_ms = std::stoull(v);
            else if (a == "--codex-home") o.codex_home = v;          // VRS_onePC_v1.md B6
            else if (a == "--migrate-claude") o.migrate_claude = v;  // B8
            else if (a == "--migrate-codex") o.migrate_codex = v;
            else if (a == "--ready-event") o.ready_event = uintptr_t(std::stoull(v));
            else throw std::runtime_error("UNKNOWN_OPTION");
        }
        if (o.data.empty() || o.endpoint.empty() || !o.workers || o.workers > 64 || !o.connections || o.connections > 64 || o.workers + o.connections > cc::cover_slots)
            throw std::runtime_error("usage: vrs_ccd [--projects ROOT] [--codex-home DIR] --data DIR --endpoint NAME [--workers 6] [--connections 16] [--read-root ROOT]");
        cc::Daemon daemon(std::move(o)); daemon.start(); daemon.serve(); return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "vrs_ccd: %.1024s\n", e.what()); return 1; }
}
