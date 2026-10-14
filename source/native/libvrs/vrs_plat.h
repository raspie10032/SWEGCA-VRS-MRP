// vrs_plat.h: the platform layer of libvrs (VRS_MCP_libvrs_v1.md 13.2, 14.4 KW1/KW2). The library's definitions do not change; only the calls to the operating
// system go through these functions. On Linux every function is the call the library made before (the Linux build is the same library). On Windows the
// same meaning is built on Win32 in vrs_plat_win.cpp (windows.h stays out of the library): no lock, no queue, no thread per call. File descriptors are kept
// as int: a Win32 kernel handle has 32 significant bits (documented for 64-bit Windows), and -1 is INVALID_HANDLE_VALUE on both sides.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <sys/types.h>

#ifdef _WIN32
// ---------------- Windows (vrs_plat_win.cpp) ----------------
// paths: the library keeps UTF-8 strings with '/' (the separator of the Linux store, so relative paths, seeds and shard names are the same strings)
std::filesystem::path vp_path(const std::string& s);
// File API boundary only. Never use this spelling as a key or path-order input.
std::filesystem::path vp_io_path(const std::string& s);
std::string vp_u8(const std::filesystem::path& p);
FILE* vp_fopen(const char* path, const char* mode);
std::string vp_weakly_canonical(const std::string& path);
// MinGW's CRT directory/canonical functions do not support extended paths.
class VpDirEntry {
  std::filesystem::path path_;
  uint32_t attributes_ = 0, tag_ = 0;
  uint64_t size_ = 0;
public:
  VpDirEntry() = default;
  VpDirEntry(std::filesystem::path p, uint32_t a, uint32_t t, uint64_t n)
      : path_(std::move(p)), attributes_(a), tag_(t), size_(n) {}
  const std::filesystem::path& path() const { return path_; }
  bool is_symlink(std::error_code& ec) const;
  bool is_directory(std::error_code& ec) const;
  bool is_regular_file(std::error_code& ec) const;
  bool is_regular_file() const { std::error_code ec; return is_regular_file(ec); }
  std::filesystem::file_status symlink_status(std::error_code& ec) const;
  uint64_t file_size(std::error_code& ec) const { ec.clear(); return size_; }
};
class vp_directory_iterator {
  struct State;
  std::shared_ptr<State> state_;
protected:
  vp_directory_iterator(const std::filesystem::path&, std::filesystem::directory_options, std::error_code&, bool recursive);
public:
  vp_directory_iterator() = default;
  explicit vp_directory_iterator(const std::filesystem::path&);
  vp_directory_iterator(const std::filesystem::path& p, std::error_code& ec)
      : vp_directory_iterator(p, std::filesystem::directory_options::none, ec, false) {}
  const VpDirEntry& operator*() const;
  const VpDirEntry* operator->() const { return &**this; }
  vp_directory_iterator& increment(std::error_code&);
  vp_directory_iterator& operator++();
  bool operator==(const vp_directory_iterator&) const;
  void disable_recursion_pending();
  vp_directory_iterator begin() const { return *this; }
  vp_directory_iterator end() const { return {}; }
};
class vp_recursive_directory_iterator : public vp_directory_iterator {
public:
  vp_recursive_directory_iterator() = default;
  explicit vp_recursive_directory_iterator(const std::filesystem::path&);
  vp_recursive_directory_iterator(const std::filesystem::path& p, std::error_code& ec)
      : vp_directory_iterator(p, std::filesystem::directory_options::none, ec, true) {}
  vp_recursive_directory_iterator(const std::filesystem::path& p, std::filesystem::directory_options o, std::error_code& ec)
      : vp_directory_iterator(p, o, ec, true) {}
};
int vp_open_ro(const char* p);  // N-A: shared asynchronous handle; vp_pread has no shared cursor
int vp_open_append(const char* p);  // shared by every thread: an asynchronous handle (no file object lock) whose writes go to the end of the file
int vp_close(int fd);
ssize_t vp_pread(int fd, void* b, size_t n, uint64_t off);  // complete this call's read before returning; handle may be shared
ssize_t vp_append(int fd, const void* b, size_t n);
bool vp_fsize(int fd, uint64_t& sz);
inline void vp_fadvise_seq(int, uint64_t) {}
void* vp_map_ro(int fd, size_t n);  // nullptr = failed
void vp_unmap(void* p, size_t n);
int vp_unlink(const char* p);
bool vp_exists(const char* p);
bool vp_is_dir(const std::string& p);
char* vp_realpath(const char* p);  // final path of an existing file or directory, UTF-8 with '/', malloc'd; nullptr when it does not exist
void vp_futex_wait(std::atomic<uint32_t>* a, uint32_t seen, int64_t ns);  // sleeps only while the word still holds `seen`; ns < 0: no limit
void vp_futex_wake_all(std::atomic<uint32_t>* a);
void vp_aw_wait(std::atomic<uint32_t>& a, uint32_t seen);  // KW1: std::atomic wait/notify is a mutex and a condition variable in this tool chain
void vp_aw_notify_one(std::atomic<uint32_t>& a);
void vp_aw_notify_all(std::atomic<uint32_t>& a);
void vp_pin_cpu(unsigned cpu);
int vp_stop_new();
void vp_stop_set(int fd);
void vp_stop_close(int fd);
// the watcher (inotify in the Linux build): ReadDirectoryChangesW on the whole tree until `stop` is set. Each changed name is handed to on_path as a UTF-8
// path with '/' (dir = a directory that was made or moved in); a lost buffer calls on_overflow (G3 witness recovery).
void vp_watch_tree(const std::string& root, int stop, void* ctx, void (*on_path)(void* ctx, const std::string& path, bool dir), void (*on_overflow)(void* ctx));
#ifndef strncasecmp
#define strncasecmp _strnicmp  // <cstring> declares it
#endif
// thread-local storage (KW2): an FLS slot per variable, taken at load; FlsGetValue reads the thread's own block (no lock). The value is made on the thread's
// first touch and deleted when the thread ends (the FLS callback), as thread_local does.
unsigned vp_tls_alloc(void (*dtor)(void*));
void* vp_tls_raw(unsigned idx);
void vp_tls_set(unsigned idx, void* p);
template <class T> void vp_tls_free(void* p) { delete static_cast<T*>(p); }
template <class T, class Tag> inline const unsigned vp_tls_idx = vp_tls_alloc(&vp_tls_free<T>);
template <class T, class Tag, class... A> T& vp_tls_get(Tag, A&&... a) {
  const unsigned idx = vp_tls_idx<T, Tag>;
  void* p = vp_tls_raw(idx);
  if (!p) { p = new T(std::forward<A>(a)...); vp_tls_set(idx, p); }
  return *static_cast<T*>(p);
}
#define VP_TLS(T, ...) vp_tls_get<T>([] {} __VA_OPT__(, ) __VA_ARGS__)
#else
// ---------------- Linux: the calls the library made before ----------------
#include <climits>
#include <ctime>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
inline std::filesystem::path vp_path(const std::string& s) { return std::filesystem::path(s); }
#define vp_io_path vp_path
#define vp_fopen std::fopen
#define vp_directory_iterator std::filesystem::directory_iterator
#define vp_recursive_directory_iterator std::filesystem::recursive_directory_iterator
inline std::string vp_u8(const std::filesystem::path& p) { return p.string(); }
inline int vp_open_ro(const char* p) { return ::open(p, O_RDONLY | O_CLOEXEC); }
inline int vp_open_append(const char* p) { return ::open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644); }
inline int vp_close(int fd) { return ::close(fd); }
inline ssize_t vp_pread(int fd, void* b, size_t n, uint64_t off) { return ::pread(fd, b, n, (off_t)off); }
inline ssize_t vp_append(int fd, const void* b, size_t n) { return ::write(fd, b, n); }
inline bool vp_fsize(int fd, uint64_t& sz) { struct stat st; if (fstat(fd, &st) != 0) return false; sz = (uint64_t)st.st_size; return true; }
inline void vp_fadvise_seq(int fd, uint64_t from) { posix_fadvise(fd, (off_t)from, 0, POSIX_FADV_SEQUENTIAL); }
inline void* vp_map_ro(int fd, size_t n) { void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0); return m == MAP_FAILED ? nullptr : m; }
inline void vp_unmap(void* p, size_t n) { ::munmap(p, n); }
inline int vp_unlink(const char* p) { return ::unlink(p); }
inline bool vp_exists(const char* p) { return ::access(p, F_OK) == 0; }
inline char* vp_realpath(const char* p) { return ::realpath(p, nullptr); }
inline void vp_futex_wait(std::atomic<uint32_t>* a, uint32_t seen, int64_t ns) {
  if (ns < 0) { syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0); return; }
  timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAIT_PRIVATE, seen, &ts, nullptr, 0);
}
inline void vp_futex_wake_all(std::atomic<uint32_t>* a) { syscall(SYS_futex, reinterpret_cast<uint32_t*>(a), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0); }
inline void vp_aw_wait(std::atomic<uint32_t>& a, uint32_t seen) { a.wait(seen); }
inline void vp_aw_notify_one(std::atomic<uint32_t>& a) { a.notify_one(); }
inline void vp_aw_notify_all(std::atomic<uint32_t>& a) { a.notify_all(); }
inline void vp_pin_cpu(unsigned cpu) { cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set); pthread_setaffinity_np(pthread_self(), sizeof set, &set); }
inline int vp_stop_new() { return ::eventfd(0, EFD_CLOEXEC); }
inline void vp_stop_set(int fd) { const uint64_t one = 1; const ssize_t w = ::write(fd, &one, sizeof one); (void)w; }
inline void vp_stop_close(int fd) { ::close(fd); }
#endif
