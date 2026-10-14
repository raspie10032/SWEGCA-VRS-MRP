// vrs_plat_win.cpp: the Windows side of vrs_plat.h (VRS_MCP_libvrs_v1.md 13.2). Every function is a direct Win32 call; none takes a lock, none queues.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <synchapi.h>
#include <memory>
#include <vector>
#include <system_error>
#include "vrs_plat.h"

namespace {
std::wstring wide(const std::string& s) {  // UTF-8 -> UTF-16
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string narrow(const wchar_t* w, size_t len) {  // UTF-16 -> UTF-8
  if (!len) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
  return s;
}
std::wstring io_name(const std::string& s) {
  std::wstring w = wide(s);
  if (w.empty() || w.compare(0, 4, L"\\\\?\\") == 0 || w.compare(0, 4, L"\\\\.\\") == 0) return w;
  std::replace(w.begin(), w.end(), L'/', L'\\');
  if (w.compare(0, 4, L"\\\\?\\") == 0 || w.compare(0, 4, L"\\\\.\\") == 0) return w;
  // Relative and drive-relative input is resolved before disabling DOS parsing.
  const DWORD capacity = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (!capacity) throw std::system_error(GetLastError(), std::system_category(), "PATH_ABSOLUTE");
  std::wstring absolute(capacity, L'\0');
  const DWORD n = GetFullPathNameW(w.c_str(), capacity, absolute.data(), nullptr);
  if (!n || n >= capacity) throw std::system_error(n ? ERROR_INSUFFICIENT_BUFFER : GetLastError(), std::system_category(), "PATH_ABSOLUTE");
  absolute.resize(n);
  if (absolute.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + absolute.substr(2);
  return L"\\\\?\\" + absolute;
}
std::wstring internal_name(std::wstring w) {
  // Inspect native spelling before filesystem::path can collapse UNC separators.
  std::replace(w.begin(), w.end(), L'/', L'\\');
  if (w.size() >= 8 && _wcsnicmp(w.c_str(), L"\\\\?\\UNC\\", 8) == 0) w = L"\\\\" + w.substr(8);
  else if (w.compare(0, 4, L"\\\\?\\") == 0) w.erase(0, 4);
  std::replace(w.begin(), w.end(), L'\\', L'/');
  return w;
}
HANDLE H(int fd) { return (HANDLE)(intptr_t)fd; }
int FD(HANDLE h) { return h == INVALID_HANDLE_VALUE || h == nullptr ? -1 : (int)(intptr_t)h; }
constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
// N-A: each thread has at most one in-flight call here; the call completes before returning.
// Retaining its event avoids a kernel-object create/close for every small read or shard frame.
struct IoEvent {
  HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~IoEvent() { if (h) CloseHandle(h); }
};
HANDLE io_event() {
  IoEvent& e = VP_TLS(IoEvent);
  if (!e.h) e.h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (e.h) ResetEvent(e.h);
  return e.h;
}
}  // namespace

std::filesystem::path vp_path(const std::string& s) { return std::filesystem::path(wide(s)); }
std::filesystem::path vp_io_path(const std::string& s) { return std::filesystem::path(io_name(s)); }
std::string vp_u8(const std::filesystem::path& p) { const auto w = internal_name(p.native()); return narrow(w.data(), w.size()); }
FILE* vp_fopen(const char* path, const char* mode) { return _wfopen(io_name(path).c_str(), wide(mode).c_str()); }
int vp_open_ro(const char* p) { return FD(CreateFileW(io_name(p).c_str(), GENERIC_READ, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr)); }
int vp_open_append(const char* p) {
  return FD(CreateFileW(io_name(p).c_str(), FILE_APPEND_DATA | SYNCHRONIZE, SHARE_ALL, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
}
int vp_close(int fd) { return CloseHandle(H(fd)) ? 0 : -1; }
ssize_t vp_pread(int fd, void* b, size_t n, uint64_t off) {
  if (n == 0) return 0;
  OVERLAPPED ov{}; ov.Offset = (DWORD)(off & 0xffffffffu); ov.OffsetHigh = (DWORD)(off >> 32);
  ov.hEvent = io_event();
  if (!ov.hEvent) return -1;
  DWORD got = 0;
  if (!ReadFile(H(fd), b, (DWORD)std::min<size_t>(n, 0x7fffffffu), &got, &ov)) {
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) return err == ERROR_HANDLE_EOF ? 0 : -1;
    // Wait for this read only, never another thread's event on the shared file handle.
    if (!GetOverlappedResult(H(fd), &ov, &got, TRUE)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
  }
  return (ssize_t)got;
}
ssize_t vp_append(int fd, const void* b, size_t n) {  // one write at the end of the file (offset all ones); the calling thread waits for its own write only
  OVERLAPPED ov{}; ov.Offset = 0xffffffffu; ov.OffsetHigh = 0xffffffffu;
  ov.hEvent = io_event();
  if (!ov.hEvent) return -1;
  DWORD put = 0;
  BOOL ok = WriteFile(H(fd), b, (DWORD)std::min<size_t>(n, 0x7fffffffu), nullptr, &ov);
  if (!ok && GetLastError() != ERROR_IO_PENDING) return -1;
  ok = GetOverlappedResult(H(fd), &ov, &put, TRUE);
  return ok ? (ssize_t)put : -1;
}
bool vp_fsize(int fd, uint64_t& sz) { LARGE_INTEGER li; if (!GetFileSizeEx(H(fd), &li)) return false; sz = (uint64_t)li.QuadPart; return true; }
void* vp_map_ro(int fd, size_t n) {
  HANDLE m = CreateFileMappingW(H(fd), nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!m) return nullptr;
  void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, n);
  CloseHandle(m);  // the view keeps the mapping
  return p;
}
void vp_unmap(void* p, size_t) { UnmapViewOfFile(p); }
int vp_unlink(const char* p) { return DeleteFileW(io_name(p).c_str()) ? 0 : -1; }
bool vp_exists(const char* p) { return GetFileAttributesW(io_name(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool vp_is_dir(const std::string& p) { const DWORD a = GetFileAttributesW(io_name(p).c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
char* vp_realpath(const char* p) {
  HANDLE h = CreateFileW(io_name(p).c_str(), 0, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  std::wstring s(1024, L'\0');
  DWORD n = GetFinalPathNameByHandleW(h, &s[0], (DWORD)s.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (n >= s.size()) { s.assign(n + 1, L'\0'); n = GetFinalPathNameByHandleW(h, &s[0], (DWORD)s.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS); }
  CloseHandle(h);
  if (n == 0 || n >= s.size()) return nullptr;
  s.resize(n);
  s = internal_name(std::move(s));
  const std::string u = narrow(s.data(), s.size());
  char* r = (char*)std::malloc(u.size() + 1);
  if (r) std::memcpy(r, u.c_str(), u.size() + 1);
  return r;
}
void vp_futex_wait(std::atomic<uint32_t>* a, uint32_t seen, int64_t ns) {
  DWORD ms = INFINITE;
  if (ns >= 0) ms = (DWORD)std::min<int64_t>((ns + 999999) / 1000000, 0x7ffffffe);
  uint32_t v = seen;
  WaitOnAddress((volatile void*)a, &v, sizeof v, ms);
}
void vp_futex_wake_all(std::atomic<uint32_t>* a) { WakeByAddressAll((void*)a); }
void vp_aw_wait(std::atomic<uint32_t>& a, uint32_t seen) {  // as std::atomic::wait: returns once the word differs from `seen`
  uint32_t v = seen;
  while (a.load(std::memory_order_seq_cst) == seen) WaitOnAddress((volatile void*)&a, &v, sizeof v, INFINITE);
}
void vp_aw_notify_one(std::atomic<uint32_t>& a) { WakeByAddressSingle((void*)&a); }
void vp_aw_notify_all(std::atomic<uint32_t>& a) { WakeByAddressAll((void*)&a); }
void vp_pin_cpu(unsigned cpu) { if (cpu < 64) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu); }
int vp_stop_new() { return FD(CreateEventW(nullptr, TRUE, FALSE, nullptr)); }
void vp_stop_set(int fd) { SetEvent(H(fd)); }
void vp_stop_close(int fd) { CloseHandle(H(fd)); }
unsigned vp_tls_alloc(void (*dtor)(void*)) { return (unsigned)FlsAlloc((PFLS_CALLBACK_FUNCTION)dtor); }
void* vp_tls_raw(unsigned idx) { return FlsGetValue((DWORD)idx); }
void vp_tls_set(unsigned idx, void* p) { FlsSetValue((DWORD)idx, p); }

void vp_watch_tree(const std::string& root, int stop, void* ctx, void (*on_path)(void*, const std::string&, bool), void (*on_overflow)(void*)) {
  HANDLE dir = CreateFileW(io_name(root).c_str(), FILE_LIST_DIRECTORY, SHARE_ALL, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return;
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ev) { CloseHandle(dir); return; }
  std::string base = root; while (base.size() > 1 && base.back() == '/') base.pop_back();
  std::unique_ptr<DWORD[]> bufw(new DWORD[(1 << 16) / sizeof(DWORD)]);
  char* buf = reinterpret_cast<char*>(bufw.get());
  const DWORD filt = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
  OVERLAPPED ov{}; bool pending = false;
  auto arm = [&] {
    ov = {}; ov.hEvent = ev; ResetEvent(ev);
    pending = ReadDirectoryChangesW(dir, buf, 1 << 16, TRUE, filt, nullptr, &ov, nullptr) != FALSE;
    return pending;
  };
  if (arm()) for (;;) {
    HANDLE hs[2] = {ev, H(stop)};
    const DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
    DWORD n = 0;
    if (w != WAIT_OBJECT_0) break;
    const BOOL ok = GetOverlappedResult(dir, &ov, &n, FALSE); pending = false;
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error != ERROR_NOTIFY_ENUM_DIR) break;
    std::vector<DWORD> completed((n + 3) / 4);
    if (n) std::memcpy(completed.data(), buf, n);
    if (!arm()) break; // Re-register before recovering or processing this completed buffer.
    if (!ok || !n) { on_overflow(ctx); continue; }
    const char* data = reinterpret_cast<const char*>(completed.data());
    for (DWORD i = 0; i < n;) {
      constexpr DWORD head = offsetof(FILE_NOTIFY_INFORMATION, FileName);
      if (n - i < head) { on_overflow(ctx); break; }
      const FILE_NOTIFY_INFORMATION* fi = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data + i);
      if (fi->FileNameLength > n - i - head || (fi->FileNameLength & 1)) { on_overflow(ctx); break; }
      std::string rel = narrow(fi->FileName, fi->FileNameLength / sizeof(WCHAR));
      for (auto& c : rel) if (c == '\\') c = '/';
      const std::string path = base + "/" + rel;
      if (fi->Action != FILE_ACTION_REMOVED && fi->Action != FILE_ACTION_RENAMED_OLD_NAME) {
        const bool made = fi->Action == FILE_ACTION_ADDED || fi->Action == FILE_ACTION_RENAMED_NEW_NAME;
        if (made && vp_is_dir(path)) on_path(ctx, path, true);
        else on_path(ctx, path, false);
      }
      if (!fi->NextEntryOffset) break;
      if (fi->NextEntryOffset < head || fi->NextEntryOffset > n - i) { on_overflow(ctx); break; }
      i += fi->NextEntryOffset;
    }
  }
  if (pending) { DWORD n; CancelIoEx(dir, &ov); GetOverlappedResult(dir, &ov, &n, TRUE); }
  CloseHandle(ev);
  CloseHandle(dir);
}
#endif
