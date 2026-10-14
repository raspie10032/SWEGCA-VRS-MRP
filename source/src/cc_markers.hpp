#pragma once
#include "cc_io.hpp"
#include "swegca_architecture/sha256.hpp"
#include "swegca_architecture/strong_types.hpp"
#ifdef _WIN32
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace cc {
inline std::string marker_hash(std::string_view bytes) {
    swegca::architecture::Sha256 h; h.update(bytes);
    return swegca::architecture::Digest256(h.finish()).hex();
}
#ifdef _WIN32
using MarkerNtStatusFn = LONG (WINAPI*)();
inline MarkerNtStatusFn marker_status_reader() {
    return reinterpret_cast<MarkerNtStatusFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetLastNtStatus"));
}
inline bool marker_retired(DWORD error, DWORD status) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
        ((error == ERROR_ACCESS_DENIED || error == 303) && (status == 0xc0000056u || status == 0xc0000123u));
}
#endif
inline std::unique_ptr<File> marker_open(const std::string& name, bool write = false) {
#ifdef _WIN32
    const auto last_status = marker_status_reader();
    HANDLE h = CreateFileW(vp_io_path(name).c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h != INVALID_HANDLE_VALUE) return std::make_unique<File>(h);
    const DWORD error = GetLastError(), status = last_status ? DWORD(last_status()) : 0;
    if (marker_retired(error, status)) return {};
    throw std::runtime_error("MARKER_OPEN_" + std::to_string(error) + "_NT_" + std::to_string(status));
#else
    auto f = std::make_unique<File>(name, write, true);
    return f->valid() ? std::move(f) : nullptr;
#endif
}
inline void marker_remove(const std::string& name) {
#ifdef _WIN32
    const auto last_status = marker_status_reader();
    HANDLE h = CreateFileW(vp_io_path(name).c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        const DWORD status = last_status ? DWORD(last_status()) : 0;
        // Another POSIX delete has set disposition and is closing its handle.
        // No caller ever cancels that deletion; both callers still recheck EOF.
        if (marker_retired(error, status)) return;
        throw std::runtime_error("MARKER_DELETE_OPEN_" + std::to_string(error) + "_NT_" + std::to_string(status));
    }
    // FileDispositionInfoEx=21. Spell the stable ABI explicitly so the fixed
    // relay's Win8 SDK target can build the Win10+ POSIX deletion capability.
    struct { DWORD flags; } disposition{0x1 | 0x2}; // DELETE | POSIX_SEMANTICS
    const BOOL ok = SetFileInformationByHandle(h, FILE_INFO_BY_HANDLE_CLASS(21), &disposition, sizeof disposition);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    const DWORD status = !ok && last_status ? DWORD(last_status()) : 0;
    CloseHandle(h);
    if (!ok && !marker_retired(error, status))
        throw std::runtime_error("MARKER_DELETE_SET_" + std::to_string(error) + "_NT_" + std::to_string(status));
#else
    if (::unlink(name.c_str()) && errno != ENOENT) throw std::runtime_error("MARKER_DELETE_" + std::to_string(errno));
#endif
}
// P is an append-completion contract, not a claim about lifecycle hooks.
inline std::string marker_name(const std::string& root, const std::string& source) {
    return root + "/" + marker_hash(source) + ".mark";
}
inline std::string marker_read(const std::string& name) {
    auto f = marker_open(name);
    if (!f) return {}; // A cover-complete peer removed it.
    const auto n = f->size();
    if (!n || n > 32768) throw std::runtime_error("MARKER_SIZE_INVALID");
    auto path = f->range(0, n);
    if (path.find('\0') != path.npos || vp_u8(vp_path(name).filename()) != marker_hash(path) + ".mark")
        throw std::runtime_error("MARKER_CONTENT_INVALID");
    return path;
}
inline void marker_publish(const std::string& root, const std::string& source) {
    const auto target = marker_name(root, source);
    unsigned char random[16];
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw std::runtime_error("MARKER_RANDOM_FAILED");
#else
    if (getrandom(random, sizeof random, 0) != sizeof random) throw std::runtime_error("MARKER_RANDOM_FAILED");
#endif
    static constexpr char hex[] = "0123456789abcdef";
    std::string tmp = root + ".tmp/";
    for (unsigned char b : random) { tmp += hex[b >> 4]; tmp += hex[b & 15]; }
#ifdef _WIN32
    HANDLE h = CreateFileW(vp_io_path(tmp).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("MARKER_TEMP_FAILED");
    DWORD put = 0; const bool ok = WriteFile(h, source.data(), DWORD(source.size()), &put, nullptr) && put == source.size();
    CloseHandle(h);
#else
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("MARKER_TEMP_FAILED");
    const bool ok = ::write(fd, source.data(), source.size()) == ssize_t(source.size());
    ::close(fd);
#endif
    struct Cleanup { std::string p; ~Cleanup() { try { marker_remove(p); } catch (...) {} } } cleanup{tmp};
    if (!ok) throw std::runtime_error("MARKER_WRITE_FAILED");
#ifdef _WIN32
    const auto last_status = marker_status_reader();
    if (CreateHardLinkW(vp_io_path(target).c_str(), vp_io_path(tmp).c_str(), nullptr)) return;
    const DWORD error = GetLastError(), status = last_status ? DWORD(last_status()) : 0;
    // Only the target can be retiring: this process alone owns the staged link.
    if ((error == ERROR_ACCESS_DENIED || error == 303) && marker_retired(error, status)) return;
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
        throw std::runtime_error("MARKER_PUBLISH_" + std::to_string(error) + "_NT_" + std::to_string(status));
#else
    std::error_code ec;
    fs::create_hard_link(vp_path(tmp), vp_path(target), ec);
    if (!ec) return;
    if (ec != std::errc::file_exists) throw std::runtime_error("MARKER_PUBLISH_FAILED");
#endif
    auto existing = marker_open(target, true);
    if (existing) {
        if (existing->size() != source.size() || existing->range(0, source.size()) != source)
            throw std::runtime_error("MARKER_HASH_COLLISION");
        existing->write_once(source, 0); // Same bytes; notify a retained partial-line marker.
    }
    // EEXIST itself linearized publication. A concurrent remover is obliged to
    // recheck source size after unlink (G2-T2), even if this read finds no name.
}
inline void marker_directories(const std::string& root) {
    fs::create_directories(vp_io_path(root));
    fs::create_directories(vp_io_path(root + ".tmp"));
}
}
