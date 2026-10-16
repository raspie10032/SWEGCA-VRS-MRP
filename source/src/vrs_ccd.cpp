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
    // ---------------- K5 3판 vrs_search (VRS_K5_query_v1.md D-r9, D-q8′, B-r1; §15.1) ----------------
    // lib7 has processed a Claude rollout up to byte end (the tail conv_tail just closed), as assembly() waits for it
    void lib_covers(vrs_h* h, const std::string& rollout, uint64_t end) {
        int rc = vrs_wait(h, rollout.c_str(), opts_.timeout_ms);
        if (rc) throw std::runtime_error("VRS_WAIT_" + std::to_string(rc));
        if (!end) return;
        rc = vrs_wait_off(h, rollout.c_str(), end, opts_.timeout_ms);
        if (rc == -2) {
            char* raw = nullptr; size_t n = 0;
            int discovered = vrs_assemble_b(h, rollout.c_str(), 258400, &raw, &n); vrs_free(raw);
            if (discovered < 0 || discovered == 2) throw std::runtime_error("VRS_ASSEMBLE_" + std::to_string(discovered));
            rc = vrs_wait_off(h, rollout.c_str(), end, opts_.timeout_ms);
        }
        if (rc) throw std::runtime_error("VRS_WAIT_OFF_" + std::to_string(rc));
    }
    using FileCache = std::unordered_map<std::string, std::unique_ptr<File>>;
    static File& opened(FileCache& cache, const std::string& path) { auto& f = cache[path]; if (!f) f.reset(new File(path)); return *f; }
    static uint64_t line_length(const File& f, uint64_t off) {  // the line at off, without its newline
        char buf[65536]; uint64_t n = 0;
        for (;;) {
            const size_t got = f.read(buf, sizeof buf, off + n);
            if (!got) throw std::runtime_error("ROLLOUT_LINE_UNTERMINATED_AT_" + std::to_string(off));
            if (const char* nl = static_cast<const char*>(std::memchr(buf, '\n', got))) return n + uint64_t(nl - buf);
            n += got;
        }
    }
    // One view entry of a Replay row: an original block (Claude: the transcript block of the converted line, sha checked; the app: the block of
    // the rollout line itself), clipped by the side's budget of code points.
    struct ViewEntry { uint64_t o = 0, n = 0, k = 0; std::string h, role, ts, text; };
    static std::string clip_points(const std::string& s, size_t& left) {
        size_t at = 0, cps = 0;
        while (at < s.size() && cps < left) { at += point_bytes(uint8_t(s[at])); ++cps; }
        left -= cps; return s.substr(0, std::min(at, s.size()));
    }
    bool app_file(const std::string& f) const { return cx_ && under(f, cx_sessions_); }
    // the original line a rollout line stands for: (original path, o) — Claude through cc (C2), the app the identity (D-u3)
    std::pair<std::string, uint64_t> origin_of(const std::string& f, const std::string& nu, uint64_t off, uint64_t len, FileCache& rollouts) {
        if (app_file(f)) return {f, off};
        const Json j = parse(opened(rollouts, f).range(off, len), true);
        return {opts_.projects + "/" + nu, j.at("payload").at("cc").at("o").uint()};
    }
    void view_of(const std::string& f, const std::string& nu, uint64_t off, uint64_t len, FileCache& rollouts, FileCache& originals, std::vector<ViewEntry>& out) {
        if (!app_file(f)) {
            const Json j = parse(opened(rollouts, f).range(off, len), true); const Json& p = j.at("payload");
            const std::string original = opts_.projects + "/" + nu;
            Restored r = restore_item(opened(originals, original), vp_u8(vp_path(original).filename()), p);
            const Json& c = p.at("cc");
            out.push_back({c.at("o").uint(), c.at("n").uint(), std::stoull(c.at("k").text()), c.at("h").text(), p.at("role").text(),
                           r.item.at("timestamp").text(), r.item.at("text").text()});
            return;
        }
        const std::string raw = opened(rollouts, f).range(off, len), hash = sha256(raw);
        const Json j = parse(raw, true);
        if (j.get("type") != "response_item") throw std::runtime_error("SOURCE_TYPE_MISMATCH_AT_" + std::to_string(off));
        const Json& q = j.at("payload"); const std::string role = q.get("role");
        if (q.get("type") != "message" || (role != "user" && role != "assistant")) throw std::runtime_error("SOURCE_ROLE_MISMATCH_AT_" + std::to_string(off));
        const Json& content = q.at("content");
        if (content.kind != Json::array) return;
        for (size_t k = 0; k < content.values.size(); ++k) {
            const Json& b = content.values[k]; const std::string bt = b.get("type");
            std::string body;
            if (bt == "input_text" || bt == "output_text") body = b.at("text").text();
            else if (bt == "input_image" && role == "user") body = "[image · " + vp_u8(vp_path(f).filename()) + "#" + std::to_string(off) + "+" + std::to_string(len) + " k=" + std::to_string(k) + "]";
            else continue;
            out.push_back({off, len, k, hash, role, j.get("timestamp"), std::move(body)});
        }
    }
    static std::string theta_text(uint64_t th) {  // θ is in 2^-20 s since 1970 (lib7 parse_ts)
        const std::time_t s = std::time_t(th >> 20); std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &s);
#else
        gmtime_r(&s, &tm);
#endif
        char b[40]; std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                                  tm.tm_sec, unsigned(((th & 0xFFFFF) * 1000) >> 20));
        return b;
    }
    static const char* phase_text(uint64_t p) { return p == 0 ? "A" : p == 1 ? "R" : "U"; }
    // D-r9′ (4판): the strength of a Replay row names its synapse σ* by the text of its cue; no σ*: strength 0
    static std::string strength_text(const std::string& w, int64_t sh, const Json& cues) {
        if (sh < 0 || size_t(sh) >= cues.values.size()) return "강도 0";
        return "강도 " + w + " (시냅스 「" + cues.values[size_t(sh)].values.at(0).text() + "」)";
    }
    // The page key of D-r9: (section, the Recall order key, the UTF-16 offset in the row) = sec:wbits:m:θₑ:o0:c:ν (ν last: it may hold ':')
    struct SKey { uint64_t sec = 0, wb = 0, m = 0, the = 0, o0 = 0, c = 0; std::string nu; };
    static uint64_t wbits(double w) { uint64_t b; std::memcpy(&b, &w, 8); return b; }  // w ≥ 0: the bits order as the values
    static SKey skey_of(const Json& row) {
        return {row.at("sec").uint(), wbits(std::strtod(row.at("w").text().c_str(), nullptr)), row.at("m").uint(), row.at("the").uint(), row.at("o0").uint(), 0, row.at("nu").text()};
    }
    static bool sbefore(const SKey& a, const SKey& b) {  // D-r7/D-r8 order inside a section, sections in turn
        if (a.sec != b.sec) return a.sec < b.sec;
        if (a.wb != b.wb) return a.wb > b.wb;
        if (a.m != b.m) return a.m > b.m;
        if (a.the != b.the) return a.the > b.the;
        if (a.nu != b.nu) { const int c = vp_path(a.nu).compare(vp_path(b.nu)); if (c) return c < 0; }
        return a.o0 < b.o0;
    }
    static std::string stoken(SKey k, uint64_t c) {
        char wb[17]; std::snprintf(wb, sizeof wb, "%016llx", (unsigned long long)k.wb);
        return std::to_string(k.sec) + ":" + wb + ":" + std::to_string(k.m) + ":" + std::to_string(k.the) + ":" + std::to_string(k.o0) + ":" + std::to_string(c) + ":" + k.nu;
    }
    static SKey skey_parse(const std::string& t) {
        std::vector<std::string_view> f; size_t a = 0;
        for (int i = 0; i < 6; ++i) { const size_t e = t.find(':', a); if (e == t.npos) throw std::runtime_error("START_EXPECT_SEARCH_KEY"); f.push_back(std::string_view(t).substr(a, e - a)); a = e + 1; }
        auto num = [](std::string_view s, int base) {
            uint64_t n = 0; auto r = std::from_chars(s.data(), s.data() + s.size(), n, base);
            if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) throw std::runtime_error("START_EXPECT_SEARCH_KEY"); return n;
        };
        SKey k{num(f[0], 10), num(f[1], 16), num(f[2], 10), num(f[3], 10), num(f[4], 10), num(f[5], 10), t.substr(a)};
        if (k.sec > 1 || k.nu.empty()) throw std::runtime_error("START_EXPECT_SEARCH_KEY");
        return k;
    }
    // Replay: the view items (the request side clipped at 1500 code points in order, the final answer at 2500), each line with ℓ
    std::string replay_body(const std::string& f, const std::string& nu, const Json& vi, FileCache& rollouts, FileCache& originals, Json& vj) {
        std::vector<ViewEntry> view;
        for (const auto& v : vi.values) view_of(f, nu, v.values.at(0).uint(), v.values.at(1).uint(), rollouts, originals, view);
        size_t req_left = 1500, fin_left = 2500;
        std::string body;
        for (auto& e : view) {
            e.text = clip_points(e.text, e.role == "user" ? req_left : fin_left);
            if (!body.empty()) body += "\n";
            body += "항목 " + std::to_string(e.o) + "+" + std::to_string(e.n) + " k=" + std::to_string(e.k) + " · " + (e.role == "user" ? "사용자" : "답") +
                    " · " + e.ts + " · sha " + e.h.substr(0, 16);
            uint64_t ell = 0;
            for (size_t a = 0; a <= e.text.size(); ++ell) {
                const size_t z = e.text.find('\n', a);
                body += "\n" + std::to_string(ell) + "│" + e.text.substr(a, z == std::string::npos ? std::string::npos : z - a);
                if (z == std::string::npos) break;
                a = z + 1;
            }
            Json x = Json::obj(); x.set("o", Json::num(e.o)).set("n", Json::num(e.n)).set("k", Json::num(e.k)).set("h", Json::str(e.h)).set("role", Json::str(e.role))
                .set("timestamp", Json::str(e.ts)).set("text", Json::str(e.text));
            vj.values.push_back(std::move(x));
        }
        return body;
    }
    // K6: a path as one key (slashes one way, case folded as Windows paths compare)
    static std::string path_key(std::string s) {
        for (auto& c : s) { if (c == '\\') c = '/'; else if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); }
        return s;
    }
    // K6 §5: the memories a prompt calls up, from the one store of both apps (VRS_K6_auto_recall_v1.md D-s1, D-s2)
    Json recall(const Json& request, unsigned slot) {
        const std::string query = request.at("query").text();
        Json out = Json::obj(); out.set("ok", Json::flag(true));
        if (query.empty()) { out.set("text", Json::str("")); return out; }
        std::vector<std::string> W; std::string caller;
        const Json res = k5_result(request, query, slot, W, &caller, true);
        const auto& rows = res.at("rows").values;
        const uint64_t Q = res.at("Q").uint(), rare = res.at("rare").uint();
        FileCache rollouts, originals;
        struct Pick { const Json* r; std::string path; uint64_t o0; };
        std::vector<Pick> picks; std::string wstar;
        for (const auto& r : rows) {
            if (r.at("sec").uint() != 1) continue;  // R in ≺ order (the Replay rows of search repeat its head)
            const std::string w = r.at("w").text();
            if (!picks.empty() && w != wstar) break;  // D-a4: the tier of w* only
            const std::string f = r.at("f").text(), nu = r.at("nu").text();
            const uint64_t ro0 = r.at("o0").uint();
            const auto org = origin_of(f, nu, ro0, line_length(opened(rollouts, f), ro0), rollouts);
            if (!caller.empty() && path_key(org.first) == path_key(caller)) continue;  // the caller's own turns
            if (picks.empty()) wstar = w;
            picks.push_back({&r, org.first, org.second});
            if (picks.size() == 5) break;
        }
        std::string text;
        if (!picks.empty()) {
            const size_t B = 10000;
            text = "⟦vrs 회상 · 이 말이 떠올린 두 앱의 기억 " + std::to_string(picks.size()) + " · 단서 " + std::to_string(Q) + " (드묾 " + std::to_string(rare) +
                   ") · 원문은 vrs_read, 더 찾으면 vrs_search⟧";
            Json items = Json::arr();
            for (size_t i = 0; i < picks.size(); ++i) {
                const Pick& p = picks[i]; const Json& r = *p.r;
                const bool app = app_file(r.at("f").text());
                const std::string stem = vp_u8(vp_path(p.path).stem());
                const std::string session = app && stem.size() >= 36 ? stem.substr(stem.size() - 36) : stem;
                const std::string head = "\n\n[" + std::string(app ? "ChatGPT 앱 · 스레드 " : "Claude Code · 세션 ") + session + " · " + theta_text(r.at("the").uint()) +
                    " · " + p.path + "#" + std::to_string(p.o0) + " · " + strength_text(r.at("w").text(), int64_t(std::stoll(r.at("sh").scalar)), res.at("cues")) +
                    " · 맞음 " + std::to_string(r.at("m").uint()) + "/" + std::to_string(Q) + "]\n";
                Json vj = Json::arr();
                std::string body = replay_body(r.at("f").text(), r.at("nu").text(), r.at("vi"), rollouts, originals, vj);
                const size_t room = utf16_units(text) + utf16_units(head);
                if (room + utf16_units(body) > B) {
                    if (i) break;  // whole memories only, after the first
                    const std::string cut = "\n[잘림]";
                    if (room + utf16_units(cut) >= B) break;
                    size_t at = 0, u = 0; const size_t left = B - room - utf16_units(cut);
                    while (at < body.size()) { const size_t pn = point_bytes(uint8_t(body[at])); const size_t pu = pn == 4 ? 2 : 1; if (u + pu > left) break; u += pu; at += pn; }
                    body = body.substr(0, at) + cut;
                }
                text += head + body;
                Json it = Json::obj(); it.set("app", Json::str(app ? "codex" : "claude")).set("session", Json::str(session)).set("path", Json::str(p.path))
                    .set("o0", Json::num(p.o0)).set("w", r.at("w")).set("m", r.at("m")).set("dmin", r.at("dmin")).set("the", r.at("the")).set("sh", r.at("sh"))
                    .set("view", std::move(vj));
                items.values.push_back(std::move(it));
            }
            out.set("items", std::move(items));
        }
        out.set("text", Json::str(text)).set("utf16", Json::num(utf16_units(text))).set("cues", res.at("cues"));
        return out;
    }
    // D-q8′ and the lib7 query of a request: W(t) and the rows (shared by search and K6 recall); caller = the caller's original file
    // recall: lib7 gives only ρ″ (vrs_recall, the caller's rollout left out), else the rows of D-r9 (vrs_search)
    Json k5_result(const Json& request, const std::string& query, unsigned slot, std::vector<std::string>& W, std::string* caller, bool recall = false) {
        vrs_h* h = ready_lib();
        // D-q8′: W(t) = the caller's file ∪ the live Claude writers' transcripts ∪ the live app writers' rollouts, each closed and in lib7
        bool first = true; std::string lib_caller;
        auto claude = [&](const std::string& transcript) {
            Entry* e = nullptr;
            const bool own = first; first = false;
            try { e = &identify(transcript); }
            catch (const std::runtime_error& x) { if (std::string_view(x.what()) == "PATH_NOT_FOUND") return; throw; }
            if (own && caller) { *caller = e->transcript; lib_caller = e->rollout; }
            if (std::find(W.begin(), W.end(), e->rollout) != W.end()) return;
            const TailResult tail = conv_tail(e->transcript, e->rollout, cover(*e), slot, h, &e->handles);
            clean_marker(*e);
            lib_covers(h, e->rollout, tail.output_end);
            W.push_back(e->rollout);
        };
        auto app = [&](const std::string& rollout) {
            const std::string r = canonical(rollout);
            if (std::find(W.begin(), W.end(), r) != W.end()) return;
            cx_core_settle(*cx_, r); W.push_back(r);
        };
        if (const auto* t = request.find("transcript")) claude(t->text());
        first = false;
        if (const auto* t = request.find("thread")) {
            if (!cx_) throw std::runtime_error("CODEX_ROOT_ABSENT");
            std::string r;
            try { r = cx_core_thread_file(*cx_, t->text()); }
            catch (const std::runtime_error& x) {  // K6 §8 KL4-3: a new thread's first prompt comes before its rollout; it has no turn to leave out
                if (!recall || std::string_view(x.what()) != "THREAD_ROLLOUT_NOT_FOUND") throw;
            }
            if (!r.empty()) {
                if (caller) *caller = r;
                lib_caller = r;
                if (std::find(W.begin(), W.end(), r) == W.end()) W.push_back(r);
            }
        }
        auto writers = [&](const std::string& root, bool own) {
            vw::scan(root, true, [&](const vw::Witness& found) {
                const auto w = vw::read(found.name); if (w.source.empty()) return;
                bool live = true;
                try { live = vw::alive(w); } catch (const std::exception&) {}
                if (live) vw::sources(w, [&](const std::string& source) { if (own) claude(source); else app(source); });
            });
        };
        writers(opts_.writer_markers, true);
        if (cx_) writers(cx_writers_, false);
        std::vector<const char*> wp; for (const auto& p : W) wp.push_back(p.c_str());
        char* raw = nullptr; size_t n = 0;
        const int rc = recall ? vrs_recall(h, query.data(), query.size(), wp.data(), unsigned(wp.size()), lib_caller.c_str(), &raw, &n)
                              : vrs_search(h, query.data(), query.size(), wp.data(), unsigned(wp.size()), &raw, &n);
        std::unique_ptr<char, decltype(&vrs_free)> bytes(raw, vrs_free);
        if (rc == 1) throw std::runtime_error("NOT_READY");
        if (rc) throw std::runtime_error("VRS_SEARCH_" + std::to_string(rc));
        return parse(std::string_view(raw, n));
    }
    Json search(const Json& request, unsigned slot) {
        ready_lib();
        const std::string query = request.at("query").text();
        if (query.empty()) throw std::runtime_error("QUERY_EMPTY");
        const size_t budget = budget_of(request);
        std::vector<std::string> W;
        const Json res = k5_result(request, query, slot, W, nullptr);
        const auto& rows = res.at("rows").values;
        const uint64_t Q = res.at("Q").uint(), rare = res.at("rare").uint(), common = res.at("common").uint(), D = res.at("D").uint();
        // the start: the first row not before the key (no request state)
        size_t begin = 0; uint64_t body_start = 0;
        if (const auto* st = request.find("start")) {
            const SKey key = skey_parse(st->text()); body_start = key.c;
            begin = size_t(std::lower_bound(rows.begin(), rows.end(), key, [](const Json& r, const SKey& k) { return sbefore(skey_of(r), k); }) - rows.begin());
            if (body_start) {
                if (begin == rows.size()) throw std::runtime_error("START_NOT_IN_RESULT");
                const SKey at = skey_of(rows[begin]);
                if (sbefore(key, at) || sbefore(at, key)) throw std::runtime_error("START_NOT_IN_RESULT");
            }
        }
        // KQ10: every cue common or no Déjà vu: the cue table under the marker
        std::string table;
        if (D == 0 || (Q && common == Q)) for (const auto& c : res.at("cues").values) {
            const uint64_t d = c.values.at(1).uint();
            table += "\n· 「" + c.values.at(0).text() + "」 df=" + (d > 64 ? std::string(">64") : std::to_string(d)) + (d == 0 ? " 없음" : d > 64 ? " 흔함" : " 드묾");
        }
        auto token = [&](size_t i, uint64_t c) { return i == rows.size() ? std::string("끝") : stoken(skey_of(rows[i]), c); };
        const std::string start = token(begin, body_start);
        auto marker = [&](size_t count, const std::string& next) {
            return "⟦vrs-search v2 · 단서 " + std::to_string(Q) + " (드묾 " + std::to_string(rare) + " · 흔함 " + std::to_string(common) + ") · Déjà vu " +
                std::to_string(D) + " · Recall " + std::to_string(res.at("R").uint()) + " · Replay " + std::to_string(res.at("rho").uint()) + " · 재검증 " +
                std::to_string(res.at("re").uint()) + " · 행 " + std::to_string(count) + "/" + std::to_string(rows.size()) + " · start=" + start + " · next=" + next + "⟧" + table;
        };
        FileCache rollouts, originals;
        struct Rendered { std::string header, body; size_t units; Json item; };
        auto render = [&](const Json& r) {
            const std::string f = r.at("f").text(), nu = r.at("nu").text();
            const bool replay = r.at("sec").uint() == 0;
            const uint64_t ro0 = r.at("o0").uint();
            // the turn range [o0, o1] in the original: its task_started line and its last line (D-r1), as line starts
            const auto first = origin_of(f, nu, ro0, line_length(opened(rollouts, f), ro0), rollouts);
            uint64_t o1 = first.second;
            if (const auto* lo = r.find("lo")) o1 = origin_of(f, nu, lo->uint(), r.at("ln").uint(), rollouts).second;
            const std::string where = nu + "#" + std::to_string(first.second);
            const std::string w = r.at("w").text(), m = std::to_string(r.at("m").uint()) + "/" + std::to_string(Q);
            const int64_t sh = int64_t(std::stoll(r.at("sh").scalar));
            Json item = Json::obj();
            item.set("sec", r.at("sec")).set("path", Json::str(first.first)).set("nu", Json::str(nu)).set("o0", Json::num(first.second)).set("o1", Json::num(o1))
                .set("open", r.at("open")).set("the", r.at("the")).set("w", Json::str(w)).set("sh", r.at("sh")).set("m", r.at("m"));
            Rendered out;
            if (!replay) {
                std::string places; Json pl = Json::arr();
                for (const auto& v : r.at("vi").values) {
                    const uint64_t vo = v.values.at(0).uint(), vn = v.values.at(1).uint();
                    std::vector<ViewEntry> ents;
                    if (app_file(f)) {
                        const Json j = parse(opened(rollouts, f).range(vo, vn), true); const Json* c = j.at("payload").find("content");
                        const size_t blocks = c && c->kind == Json::array ? c->values.size() : 0;
                        for (size_t b = 0; b < blocks; ++b) ents.push_back({vo, vn, b, {}, {}, {}, {}});
                    } else {
                        const Json j = parse(opened(rollouts, f).range(vo, vn), true); const Json& c = j.at("payload").at("cc");
                        ents.push_back({c.at("o").uint(), c.at("n").uint(), std::stoull(c.at("k").text()), {}, {}, {}, {}});
                    }
                    for (const auto& e : ents) {
                        places += " (" + std::to_string(e.o) + ", " + std::to_string(e.k) + ")";
                        Json a = Json::arr(); a.values.push_back(Json::num(e.o)); a.values.push_back(Json::num(e.k)); pl.values.push_back(std::move(a));
                    }
                }
                out.header = "[" + w + " · 맞음 " + m + " · " + theta_text(r.at("the").uint()) + " · " + nu + "#" + std::to_string(first.second) + "–" +
                             std::to_string(o1) + (r.at("open").truth() ? " · 열림" : "") + " · 항목" + places + "]";
                item.set("items", std::move(pl));
                out.units = 0; out.item = std::move(item);
                return out;
            }
            Json vj = Json::arr();
            std::string body = replay_body(f, nu, r.at("vi"), rollouts, originals, vj);
            const uint64_t pq = r.at("pq").uint(), pe = r.at("pe").uint();
            std::string re;
            if (r.at("re").uint() && !r.at("n").uint()) re = " · 재검증 (질의 관측이 사전 기권: 창·보기 줄·출처·규칙)";  // D-r5: no evidence to show
            else if (r.at("re").uint()) {
                std::string hits; for (const auto& x : r.at("hits").values) hits += (hits.empty() ? "" : ",") + std::to_string(x.uint());
                re = " · 재검증 (맞은 보기 줄 " + std::to_string(r.at("sq").uint()) + "/" + std::to_string(r.at("n").uint()) + " · τ′ " + r.at("tau").text() +
                     " · 줄 " + (hits.empty() ? "-" : hits) + ")";
                item.set("sq", r.at("sq")).set("n", r.at("n")).set("tau", r.at("tau")).set("hits", r.at("hits"));
            }
            out.header = "[" + theta_text(r.at("the").uint()) + " · " + where + " · " + strength_text(w, sh, res.at("cues")) + " · 맞음 " + m + " · 입력 " + phase_text(pq) +
                         " / 경험 " + phase_text(pe) + re + (r.at("open").truth() ? " · 열림" : "") + "]\n";
            item.set("pq", r.at("pq")).set("pe", r.at("pe")).set("re", r.at("re")).set("view", std::move(vj));
            out.units = utf16_units(body); out.body = std::move(body); out.item = std::move(item);
            return out;
        };
        // pages as vrs_restore: a position on the row sequence (row key + UTF-16 body offset c); no request state
        size_t end = begin, used = 0; uint64_t c = body_start;
        std::string fragments; Json items = Json::arr();
        while (end < rows.size()) {
            Rendered rd = render(rows[end]);
            const std::string& body = rd.body;
            const size_t byte_start = body_byte_at(body, c);
            const std::string continuation = "[이어짐 c=" + std::to_string(c) + " · " + rd.header.substr(1);
            std::string head = c ? continuation : rd.header;
            const std::string after = token(end + 1, 0);
            size_t take = body.size() - byte_start, take_units = rd.units - size_t(c);
            const bool whole = used + 2 + utf16_units(head) + take_units + utf16_units(marker(items.values.size() + 1, after)) <= budget;
            if (!whole) {
                head = continuation; take = 0; take_units = 0;
                const size_t overhead = used + 2 + utf16_units(head);
                size_t at = byte_start, u = 0;
                while (at < body.size()) {
                    const size_t pn = point_bytes(uint8_t(body[at])); u += pn == 4 ? 2 : 1; at += pn;
                    if (at == body.size()) break;
                    if (overhead + u + utf16_units(marker(items.values.size() + 1, token(end, c + u))) > budget) break;
                    take = at - byte_start; take_units = u;
                }
                if (!take) {
                    if (items.values.empty()) throw std::runtime_error("PAGE_BUDGET_TOO_SMALL_AT_" + token(end, c));
                    break;
                }
            }
            Json item = std::move(rd.item);
            item.set("text", Json::str(body.substr(byte_start, take))).set("c", Json::num(c));
            items.values.push_back(std::move(item)); fragments += "\n\n" + head + body.substr(byte_start, take);
            used += 2 + utf16_units(head) + take_units;
            if (!whole) { c += take_units; break; }
            ++end; c = 0;
        }
        const std::string next = token(end, c);
        std::string text = marker(items.values.size(), next) + fragments;
        if (utf16_units(text) > budget) throw std::runtime_error("MARKER_EXCEEDS_BUDGET");
        Json out = Json::obj(); out.set("ok", Json::flag(true)).set("text", Json::str(text)).set("items", std::move(items))
            .set("total", Json::num(rows.size())).set("next", end == rows.size() ? Json{} : Json::str(next)).set("utf16", Json::num(utf16_units(text)));
        for (const char* f : {"Q", "rare", "common", "D", "R", "rho", "re", "reads", "open", "us"}) out.set(f, res.at(f));
        Json w = Json::arr(); for (const auto& p : W) w.values.push_back(Json::str(p)); out.set("W", std::move(w));
        return out;
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
            if (op == "recall") return recall(j, slot).dump() + '\n';  // K6 §5: before the thread route (a recall may name a thread)
            if (op == "search") {  // K5 D-r9: either bridge (a transcript or a thread names the caller's file of W)
                const auto began = std::chrono::steady_clock::now();
                std::string reply = search(j, slot).dump() + '\n';
                if (opts_.test_metrics) std::fprintf(stderr, "VRS_TEST_SEARCH us=%lld elapsed_us=%lld\n",
                    (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count(), elapsed_us());
                return reply;
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
            if (opts_.test_metrics && op == "k5_stats") {  // T3-6 / T3-9: the K5 layer's state and byte count (vrs_k5_stats)
                vrs_h* h = lib(); vrs_k5dbg d{}; if (h) vrs_k5_stats(h, &d);
                Json out = Json::obj(); out.set("ok", Json::flag(true)).set("open", Json::flag(h != nullptr)).set("lib_ready", Json::flag(h && vrs_ready(h) == 1))
                    .set("k5_ready", Json::flag(h && vrs_k5_ready(h) == 1)).set("pend", Json::num(uint64_t(std::max<int64_t>(0, d.pend))))
                    .set("size", Json::num(d.size)).set("base_entries", Json::num(d.base_entries)).set("node_bytes", Json::num(d.node_bytes))
                    .set("unit_bytes", Json::num(d.unit_bytes)).set("units_built", Json::num(d.units_built)).set("units_restored", Json::num(d.units_restored))
                    .set("claimed", Json::num(d.claimed)).set("shadow_bad", Json::num(d.shadow_bad)).set("syn_bytes", Json::num(d.syn_bytes)).set("syn_miss", Json::num(d.syn_miss))
                    .set("ex_total", Json::num(d.ex_total)).set("ex_bad", Json::num(d.ex_bad)).set("ready_us", Json::num(uint64_t(std::max<int64_t>(0, d.ready_us))))
                    .set("elapsed_us", Json::num(uint64_t(elapsed_us())));
                return out.dump() + '\n';
            }
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
