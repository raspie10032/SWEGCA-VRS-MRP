#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "vrs_plat.h"
#include "vrs_handle_cache.h"
#include "cc_json.hpp"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <stdexcept>

namespace cc {
namespace fs = std::filesystem;
inline std::string canonical(const std::string& path) {
    char* p = vp_realpath(path.c_str());
    if (!p) throw std::runtime_error("PATH_NOT_FOUND");
    std::string out(p); std::free(p); return out;
}
inline bool under(const std::string& path, const std::string& root) {
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
}
#ifdef _WIN32
struct IoEvent {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    IoEvent() { if (!handle) throw std::runtime_error("IO_EVENT_FAILED"); }
    ~IoEvent() { CloseHandle(handle); }
};
inline HANDLE io_event() {
    // Every call finishes its OVERLAPPED before returning. FLS keeps events
    // separate between threads without per-operation kernel object creation.
    auto& event = VP_TLS(IoEvent);
    if (!ResetEvent(event.handle)) throw std::runtime_error("IO_EVENT_RESET_FAILED");
    return event.handle;
}
#endif
struct File {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
#ifdef _WIN32
    explicit File(HANDLE opened) : h(opened) {} // Adopt one already validated native handle.
#endif
    explicit File(const std::string& path, bool write = false, bool optional = false) {
#ifdef _WIN32
        h = CreateFileW(vp_io_path(path).c_str(), write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, write && !optional ? OPEN_ALWAYS : OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        bool missing = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
#else
        fd = ::open(path.c_str(), (write ? O_RDWR | (optional ? 0 : O_CREAT) : O_RDONLY) | O_CLOEXEC, 0600);
        bool missing = errno == ENOENT;
#endif
        if (!valid() && !(optional && missing)) throw std::runtime_error("FILE_OPEN_FAILED");
    }
    bool valid() const {
#ifdef _WIN32
        return h != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }
    int native_fd() const {
#ifdef _WIN32
        return int(intptr_t(h));
#else
        return fd;
#endif
    }
    ~File() {
#ifdef _WIN32
        if (valid()) CloseHandle(h);
#else
        if (valid()) ::close(fd);
#endif
    }
    File(const File&) = delete;
    void check_source(const std::string& path) const {
#ifdef _WIN32
        FILE_STANDARD_INFO info{};
        if (!GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof info) || info.DeletePending)
            throw std::runtime_error("SOURCE_REPLACED");
        std::wstring name(32768, L'\0');
        DWORD n = GetFinalPathNameByHandleW(h, name.data(), DWORD(name.size()), FILE_NAME_NORMALIZED);
        if (!n || n >= name.size()) throw std::runtime_error("SOURCE_IDENTITY_FAILED");
        name.resize(n); name = vp_path(vp_u8(fs::path(name))).wstring();
        std::wstring expected = vp_path(path).wstring();
        std::replace(name.begin(), name.end(), L'\\', L'/');
        std::replace(expected.begin(), expected.end(), L'\\', L'/');
        if (_wcsicmp(name.c_str(), expected.c_str())) throw std::runtime_error("SOURCE_REPLACED");
#else
        struct stat a{}, b{};
        if (fstat(fd, &a) || stat(path.c_str(), &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino)
            throw std::runtime_error("SOURCE_REPLACED");
#endif
    }
    // Establish the fixed cover extent without moving a file pointer or writing
    // zeroes over another thread's slot. Concurrent calls choose the same size.
    void cover_extent() const {
        const uint64_t n = size();
        if (n > 512) throw std::runtime_error("COVER_SIZE_INVALID");
        if (n == 512) return;
#ifdef _WIN32
        FILE_END_OF_FILE_INFO end{}; end.EndOfFile.QuadPart = 512;
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof end)) throw std::runtime_error("COVER_SIZE_FAILED");
#else
        if (::ftruncate(fd, 512)) throw std::runtime_error("COVER_SIZE_FAILED");
#endif
    }
    uint64_t size() const {
        if (!valid()) return 0;
#ifdef _WIN32
        LARGE_INTEGER n; if (!GetFileSizeEx(h, &n)) throw std::runtime_error("FILE_SIZE_FAILED"); return uint64_t(n.QuadPart);
#else
        struct stat st; if (fstat(fd, &st)) throw std::runtime_error("FILE_SIZE_FAILED");
        if (!S_ISREG(st.st_mode)) throw std::runtime_error("FILE_NOT_REGULAR"); return uint64_t(st.st_size);
#endif
    }
    size_t read(void* out, size_t n, uint64_t offset) const {
        if (!n) return 0;
#ifdef _WIN32
        OVERLAPPED ov{}; ov.Offset = DWORD(offset); ov.OffsetHigh = DWORD(offset >> 32);
        ov.hEvent = io_event();
        DWORD got = 0; BOOL ok = ReadFile(h, out, DWORD(std::min<size_t>(n, 0x7fffffff)), nullptr, &ov);
        DWORD err = ok ? 0 : GetLastError();
        if (ok || err == ERROR_IO_PENDING) { ok = GetOverlappedResult(h, &ov, &got, TRUE); err = ok ? 0 : GetLastError(); }
        if (!ok && err != ERROR_HANDLE_EOF) throw std::runtime_error("READ_FAILED"); return got;
#else
        ssize_t got; do { got = ::pread(fd, out, n, off_t(offset)); } while (got < 0 && errno == EINTR);
        if (got < 0) throw std::runtime_error("READ_FAILED"); return size_t(got);
#endif
    }
    std::string range(uint64_t off, uint64_t n) const {
        if (off > uint64_t(INT64_MAX) || n > SIZE_MAX || n > uint64_t(INT64_MAX) - off) throw std::runtime_error("RANGE_OVERFLOW");
        std::string out(size_t(n), '\0'); size_t at = 0;
        while (at < out.size()) { size_t k = read(out.data() + at, out.size() - at, off + at); if (!k) throw std::runtime_error("SHORT_READ"); at += k; }
        return out;
    }
    // One positioned operation: any short write is reported and its complete prefix
    // remains recoverable by the next conversion. No truncation races with peers.
    void write_once(std::string_view bytes, uint64_t off) const {
        if (bytes.empty()) return;
#ifdef _WIN32
        if (bytes.size() > 0x7fffffff) throw std::runtime_error("WRITE_TOO_LARGE");
        OVERLAPPED ov{}; ov.Offset = DWORD(off); ov.OffsetHigh = DWORD(off >> 32);
        ov.hEvent = io_event();
        DWORD put = 0; BOOL ok = WriteFile(h, bytes.data(), DWORD(bytes.size()), nullptr, &ov);
        if (ok || GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &put, TRUE);
        if (!ok || put != bytes.size()) throw std::runtime_error("SHORT_WRITE");
#else
        const ssize_t put = ::pwrite(fd, bytes.data(), bytes.size(), off_t(off));
        if (put < 0 || size_t(put) != bytes.size()) throw std::runtime_error("SHORT_WRITE");
#endif
    }
};
// Entry owns only identity. C=8 per-thread cache retains reusable handles;
// callers hold a lease through each I/O scope, including nested cache misses.
class FileSlot {
    const std::shared_ptr<VrsHandleKey> key_ = std::make_shared<VrsHandleKey>();
public:
    std::shared_ptr<File> get(const std::string& path, bool write, bool optional = false) {
        return VrsHandleCache<File>::local().get(key_, [&]() -> std::shared_ptr<File> {
            if (write && !optional) fs::create_directories(vp_io_path(path).parent_path());
            auto file = std::make_shared<File>(path, write, optional);
            if (!file->valid()) return {};
            if (!write && !key_->accepts(file->native_fd())) throw std::runtime_error("SOURCE_REPLACED");
            return file;
        });
    }
};
struct EntryFiles {
    FileSlot source, output, cover;
};
// Walk backwards with one read per 64 KiB. A trailing unterminated line is absent.
class ReverseLines {
    const File& file_; uint64_t base_, cursor_; std::string buf_;
public:
    explicit ReverseLines(const File& f, uint64_t end) : file_(f), base_(end), cursor_(end) {
        std::string ignored; uint64_t at; previous(ignored, at, false);
    }
    bool previous(std::string& out, uint64_t& offset, bool terminated = true) {
        out.clear(); std::vector<std::string> parts;
        uint64_t end = cursor_;
        for (;;) {
            if (cursor_ == base_ && cursor_) {
                uint64_t a = cursor_ > 65536 ? cursor_ - 65536 : 0;
                buf_ = file_.range(a, cursor_ - a); base_ = a;
            }
            if (!cursor_) {
                offset = 0; for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += *it;
                return terminated && end > 0;
            }
            size_t e = size_t(cursor_ - base_);
            size_t p = buf_.rfind('\n', e - 1);
            size_t a = p == std::string::npos ? 0 : p + 1;
            parts.push_back(buf_.substr(a, e - a)); cursor_ = base_ + a;
            if (p != std::string::npos) {
                offset = cursor_; --cursor_;
                for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += *it;
                return terminated;
            }
        }
    }
};
inline void wake_word(std::atomic<uint32_t>& word) {
    word.fetch_add(1, std::memory_order_seq_cst);
#ifdef _WIN32
    vp_aw_notify_all(word);
#else
    vp_futex_wake_all(&word);
#endif
}
inline void sleep_word(std::atomic<uint32_t>& word, uint32_t seen) {
#ifdef _WIN32
    vp_aw_wait(word, seen);
#else
    vp_futex_wait(&word, seen, -1);
#endif
}
}
