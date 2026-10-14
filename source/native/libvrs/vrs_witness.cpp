#include "vrs_witness.h"
#include "../../src/cc_markers.hpp"
#include <fstream>
#include <sstream>
#include <unordered_set>
#ifdef _WIN32
#include <winternl.h>
#else
#include <unistd.h>
#endif

namespace vw {
namespace fs = std::filesystem;
#ifdef _WIN32
using Query = LONG (WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
static Query query() {
    auto f = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!f) throw std::runtime_error("PROCESS_QUERY_UNAVAILABLE");
    return f;
}
struct Handle { HANDLE h; ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
#endif
Process process(uint64_t pid) {
    Process p; p.pid = pid;
    if (!pid || pid > UINT32_MAX) throw std::runtime_error("WRITER_PID_INVALID");
#ifdef _WIN32
    Handle h{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid))};
    if (!h.h) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return p;
        throw std::runtime_error("WRITER_PROCESS_OPEN_" + std::to_string(GetLastError()));
    }
    FILETIME c{}, e{}, k{}, u{}; DWORD status;
    if (!GetProcessTimes(h.h, &c, &e, &k, &u) || !GetExitCodeProcess(h.h, &status))
        throw std::runtime_error("WRITER_PROCESS_TIMES_FAILED");
    p.birth = (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    p.alive = status == STILL_ACTIVE;
    // Same stable ABI as PROCESS_BASIC_INFORMATION, including the parent PID.
    struct Basic { LONG exit; PVOID peb; ULONG_PTR affinity; LONG priority; ULONG_PTR pid, parent; } b{};
    if (query()(h.h, 0, &b, sizeof b, nullptr) < 0) throw std::runtime_error("WRITER_PARENT_QUERY_FAILED");
    p.parent = b.parent;
#else
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    if (!in) {
        if (errno == ENOENT || errno == ESRCH) return p;
        throw std::runtime_error("WRITER_PROCESS_STAT_FAILED");
    }
    std::string raw; std::getline(in, raw); const auto end = raw.rfind(')');
    if (end == raw.npos) throw std::runtime_error("WRITER_PROCESS_STAT_INVALID");
    std::istringstream fields(raw.substr(end + 2)); std::string value; char state;
    fields >> state >> p.parent;
    for (unsigned field = 5; field <= 21; ++field) fields >> value;
    if (!(fields >> p.birth)) throw std::runtime_error("WRITER_PROCESS_STAT_INVALID");
    p.alive = state != 'Z' && state != 'X' && state != 'x';
#endif
    return p;
}
std::string boot() {
#ifdef _WIN32
    return "windows-filetime"; // Creation FILETIME includes the date, not a boot-relative tick.
#else
    std::ifstream in("/proc/sys/kernel/random/boot_id"); std::string id;
    if (!(in >> id) || id.size() != 36) throw std::runtime_error("WRITER_BOOT_ID_FAILED");
    return id;
#endif
}
bool alive(const Witness& w) {
    if (w.boot != boot()) return false;
    const auto p = process(w.pid);
    return p.alive && p.birth == w.birth;
}
std::string path(const std::string& p) {
    if (p.empty()) throw std::runtime_error("WRITER_PATH_EMPTY");
#ifdef _WIN32
    return vp_weakly_canonical(p);
#else
    std::error_code ec;
    auto c = fs::weakly_canonical(fs::absolute(vp_io_path(p)), ec);
    if (ec) throw std::runtime_error("WRITER_PATH_INVALID");
    auto out = vp_u8(c); std::replace(out.begin(), out.end(), '\\', '/');
    return out;
#endif
}
std::string encode(const Witness& w) {
    return "VRSW1\n" + std::to_string(w.pid) + "\n" + std::to_string(w.birth) + "\n" + w.boot + "\n" + w.source;
}
Witness read(const std::string& name) {
    auto f = cc::marker_open(name); if (!f) return {};
    const auto n = f->size(); if (n < 8 || n > 32768) throw std::runtime_error("WRITER_MARKER_SIZE_INVALID");
    const std::string text = f->range(0, n);
    if (vp_u8(vp_path(name).filename()) != cc::marker_hash(text) + ".mark") throw std::runtime_error("WRITER_MARKER_HASH_INVALID");
    if (text.compare(0, 6, "VRSW1\n")) throw std::runtime_error("WRITER_MARKER_VERSION_INVALID");
    Witness w; w.name = name; size_t pos = 6;
    auto field = [&] { const auto end = text.find('\n', pos); if (end == text.npos) throw std::runtime_error("WRITER_MARKER_FIELDS_INVALID"); auto s = text.substr(pos, end - pos); pos = end + 1; return s; };
    auto number = [&](const std::string& s) { uint64_t v; auto r = std::from_chars(s.data(), s.data() + s.size(), v); if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !v) throw std::runtime_error("WRITER_MARKER_NUMBER_INVALID"); return v; };
    w.pid = number(field()); w.birth = number(field()); w.boot = field(); w.source = text.substr(pos);
    if (w.source.empty() || w.source.find('\0') != w.source.npos || vp_path(w.source).extension() != ".jsonl") throw std::runtime_error("WRITER_MARKER_SOURCE_INVALID");
    return w;
}
void publish(const std::string& root, const Witness& w) {
    const auto bytes = encode(w);
    if (auto current = cc::marker_open(cc::marker_name(root, bytes))) {
        if (current->size() != bytes.size() || current->range(0, bytes.size()) != bytes) throw std::runtime_error("WRITER_MARKER_COLLISION");
        return;
    }
    cc::marker_directories(root);
    cc::marker_publish(root, bytes); // G2's immutable, content-addressed, idempotent publication.
}
uint64_t scan(const std::string& root, bool writers, const std::function<void(const Witness&)>& visit) {
    std::error_code ec; vp_directory_iterator it(vp_io_path(root), ec), end;
    if (ec == std::errc::no_such_file_or_directory) return 0;
    if (ec) throw std::runtime_error("WITNESS_ENUM_FAILED");
    uint64_t count = 0;
    for (; it != end; it.increment(ec)) {
        if (ec) throw std::runtime_error("WITNESS_ENUM_FAILED");
        ++count; const auto status = it->symlink_status(ec);
        if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) { ec.clear(); continue; }
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) throw std::runtime_error("WITNESS_ENTRY_INVALID");
        const auto name = vp_u8(it->path()); if (it->path().extension() != ".mark") continue;
        Witness w;
        if (writers) w = read(name); else { w.source = cc::marker_read(name); w.name = name; }
        if (!w.source.empty()) visit(w);
    }
    if (ec) throw std::runtime_error("WITNESS_ENUM_FAILED");
    return count;
}
uint64_t sources(const Witness& w, const std::function<void(const std::string&)>& visit) {
    uint64_t count = 0; std::error_code ec;
    auto add = [&](const std::string& p) { ++count; visit(p); };
    // Unborn source: retained W will discover it on the next observation.
    if (fs::exists(vp_io_path(w.source), ec)) add(w.source);
    if (ec) throw std::runtime_error("WRITER_SOURCE_STATUS_FAILED");
    if (!w.pid) return count; // M names exactly one file.
    auto folder = vp_io_path(w.source); folder.replace_extension();
    const auto root_status = fs::symlink_status(folder, ec);
    if (ec == std::errc::no_such_file_or_directory || root_status.type() == fs::file_type::not_found) return count;
    if (ec) throw std::runtime_error("WRITER_SESSION_STATUS_FAILED");
    if (fs::is_symlink(root_status) || !fs::is_directory(root_status)) return count;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("WRITER_SESSION_STATUS_FAILED");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return count;
#endif
    vp_recursive_directory_iterator it(folder, ec), end;
    if (ec == std::errc::no_such_file_or_directory) return count;
    if (ec) throw std::runtime_error("WRITER_SESSION_ENUM_FAILED");
    for (; it != end; it.increment(ec)) {
        if (ec) throw std::runtime_error("WRITER_SESSION_ENUM_FAILED");
        const auto status = it->symlink_status(ec); if (ec) throw std::runtime_error("WRITER_SESSION_STATUS_FAILED");
        if (fs::is_symlink(status)) { it.disable_recursion_pending(); continue; }
        if (fs::is_regular_file(status) && it->path().extension() == ".jsonl") add(vp_u8(it->path()));
    }
    if (ec) throw std::runtime_error("WRITER_SESSION_ENUM_FAILED");
    return count;
}
Witness owner(const std::string& source, uint64_t pid) {
    const auto p = process(pid);
    if (!p.alive || !p.birth) throw std::runtime_error("WRITER_NOT_ALIVE");
    return Witness{path(source), boot(), {}, pid, p.birth};
}
static bool writes(uint64_t pid, const std::string& source) {
#ifdef _WIN32
    Handle process_handle{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, DWORD(pid))};
    if (!process_handle.h) throw std::runtime_error("WRITER_HANDLES_OPEN_FAILED");
    struct Item { HANDLE handle; ULONG_PTR handles, pointers; ULONG access, type, attributes, reserved; };
    struct Snapshot { ULONG_PTR count, reserved; Item items[1]; };
    std::vector<unsigned char> bytes(65536); ULONG needed = 0;
    LONG status = query()(process_handle.h, 51, bytes.data(), ULONG(bytes.size()), &needed);
    if (status == LONG(0xc0000004u) || status == LONG(0xc0000023u)) {
        bytes.resize(size_t(needed) + 65536); // Capacity growth, not a completion wait/retry loop.
        status = query()(process_handle.h, 51, bytes.data(), ULONG(bytes.size()), &needed);
    }
    if (status < 0) throw std::runtime_error("WRITER_HANDLES_QUERY_FAILED");
    const auto* snapshot = reinterpret_cast<const Snapshot*>(bytes.data());
    if (snapshot->count > (bytes.size() - offsetof(Snapshot, items)) / sizeof(Item)) throw std::runtime_error("WRITER_HANDLES_INVALID");
    const auto expected = vp_path(source).wstring();
    for (size_t i = 0; i < snapshot->count; ++i) {
        const auto& item = snapshot->items[i]; if (!(item.access & (FILE_WRITE_DATA | FILE_APPEND_DATA))) continue;
        HANDLE copied;
        if (!DuplicateHandle(process_handle.h, item.handle, GetCurrentProcess(), &copied, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        Handle file{copied}; if (GetFileType(file.h) != FILE_TYPE_DISK) continue;
        std::wstring name(32768, '\0'); DWORD n = GetFinalPathNameByHandleW(file.h, name.data(), DWORD(name.size()), FILE_NAME_NORMALIZED);
        if (!n || n >= name.size()) continue;
        name.resize(n); name = vp_path(vp_u8(fs::path(name))).wstring();
        std::replace(name.begin(), name.end(), L'\\', L'/');
        if (!_wcsicmp(name.c_str(), expected.c_str())) return true;
    }
#else
    std::error_code ec;
    const auto root = "/proc/" + std::to_string(pid);
    for (vp_directory_iterator it(root + "/fd", ec), end; !ec && it != end; it.increment(ec)) {
        auto target = fs::read_symlink(it->path(), ec); if (ec) { ec.clear(); continue; }
        if (target.string() != source) continue;
        std::ifstream info(root + "/fdinfo/" + it->path().filename().string()); std::string key, value;
        while (info >> key >> value) if (key == "flags:") {
            auto flags = std::stoull(value, nullptr, 8);
            if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) return true;
        }
    }
    if (ec) throw std::runtime_error("WRITER_HANDLES_QUERY_FAILED");
#endif
    return false;
}
Witness hook_owner(const std::string& source, const std::string& session, const std::string& host, const std::string& claude_home) {
    const auto f = path(source);
#ifdef _WIN32
    uint64_t pid = GetCurrentProcessId();
#else
    uint64_t pid = uint64_t(getpid());
#endif
    std::unordered_set<uint64_t> seen;
    for (Process child = process(pid); child.parent && seen.insert(child.parent).second;) {
        const auto p = process(child.parent); if (!p.alive) break;
        if (p.birth > child.birth) throw std::runtime_error("WRITER_ANCESTOR_REUSED");
        bool match = false;
        if (host == "claude") {
            const auto name = claude_home + "/sessions/" + std::to_string(p.pid) + ".json";
            cc::File record(name, false, true);
            if (record.valid()) {
                if (record.size() > 1024 * 1024) throw std::runtime_error("WRITER_SESSION_RECORD_TOO_LARGE");
                match = cc::parse(record.range(0, record.size())).get("sessionId") == session;
            }
        } else if (host == "codex") match = writes(p.pid, f);
        else throw std::runtime_error("WRITER_HOST_INVALID");
        if (match) return Witness{f, boot(), {}, p.pid, p.birth};
        child = p;
    }
    throw std::runtime_error("WRITER_ANCESTOR_NOT_FOUND");
}
}
