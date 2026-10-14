// KL3: native name enumeration. No CRT directory or canonical name calls.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "vrs_plat.h"
#include <vector>

namespace fs = std::filesystem;
bool VpDirEntry::is_symlink(std::error_code& ec) const {
  ec.clear();
  return (attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) &&
      (tag_ == IO_REPARSE_TAG_SYMLINK || tag_ == IO_REPARSE_TAG_MOUNT_POINT);
}
bool VpDirEntry::is_directory(std::error_code& ec) const { ec.clear(); return attributes_ & FILE_ATTRIBUTE_DIRECTORY; }
bool VpDirEntry::is_regular_file(std::error_code& ec) const { ec.clear(); return !(attributes_ & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)); }
fs::file_status VpDirEntry::symlink_status(std::error_code& ec) const {
  ec.clear();
  return fs::file_status(is_symlink(ec) ? fs::file_type::symlink :
      is_directory(ec) ? fs::file_type::directory : fs::file_type::regular);
}
struct vp_directory_iterator::State {
  struct Frame {
    HANDLE h;
    std::string root;
    WIN32_FIND_DATAW data{};
    bool fresh = true;
    Frame(HANDLE f, std::string p, WIN32_FIND_DATAW d) : h(f), root(std::move(p)), data(d) {}
    ~Frame() { FindClose(h); }
  };
  std::vector<std::unique_ptr<Frame>> stack;
  VpDirEntry entry;
  fs::directory_options options;
  bool recursive, descend = true;
  State(fs::directory_options o, bool r) : options(o), recursive(r) {}
  bool open(const std::string& root, std::error_code& ec) {
    WIN32_FIND_DATAW data{};
    const auto name = vp_io_path(root + "/*");
    HANDLE h = FindFirstFileExW(name.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || (error == ERROR_ACCESS_DENIED &&
          (options & fs::directory_options::skip_permission_denied) != fs::directory_options::none)) return false;
      ec = std::error_code(error, std::system_category()); return false;
    }
    stack.emplace_back(new Frame(h, root, data)); return true;
  }
  void next(std::error_code& ec) {
    while (!stack.empty()) {
      auto& f = *stack.back();
      if (!f.fresh && !FindNextFileW(f.h, &f.data)) {
        const DWORD error = GetLastError(); stack.pop_back();
        if (error != ERROR_NO_MORE_FILES) { ec = std::error_code(error, std::system_category()); stack.clear(); return; }
        continue;
      }
      f.fresh = false;
      if (!wcscmp(f.data.cFileName, L".") || !wcscmp(f.data.cFileName, L"..")) continue;
      entry = VpDirEntry(vp_path(f.root + "/" + vp_u8(fs::path(f.data.cFileName))),
          f.data.dwFileAttributes, f.data.dwReserved0,
          (uint64_t(f.data.nFileSizeHigh) << 32) | f.data.nFileSizeLow);
      return;
    }
  }
};
vp_directory_iterator::vp_directory_iterator(const fs::path& p, fs::directory_options o, std::error_code& ec, bool recursive) {
  ec.clear(); auto s = std::make_shared<State>(o, recursive);
  if (s->open(vp_u8(p), ec)) { s->next(ec); if (!ec && !s->stack.empty()) state_ = std::move(s); }
}
vp_directory_iterator::vp_directory_iterator(const fs::path& p) {
  std::error_code ec; *this = vp_directory_iterator(p, ec);
  if (ec) throw fs::filesystem_error("directory_iterator", p, ec);
}
vp_recursive_directory_iterator::vp_recursive_directory_iterator(const fs::path& p) {
  std::error_code ec; *this = vp_recursive_directory_iterator(p, ec);
  if (ec) throw fs::filesystem_error("recursive_directory_iterator", p, ec);
}
const VpDirEntry& vp_directory_iterator::operator*() const { return state_->entry; }
bool vp_directory_iterator::operator==(const vp_directory_iterator& other) const { return state_ == other.state_; }
void vp_directory_iterator::disable_recursion_pending() { if (state_) state_->descend = false; }
vp_directory_iterator& vp_directory_iterator::increment(std::error_code& ec) {
  ec.clear(); if (!state_) return *this;
  auto& s = *state_;
  if (s.recursive && s.descend && s.entry.is_directory(ec) && !s.entry.is_symlink(ec))
    s.open(vp_u8(s.entry.path()), ec);
  s.descend = true;
  if (!ec) s.next(ec);
  if (ec || s.stack.empty()) state_.reset();
  return *this;
}
vp_directory_iterator& vp_directory_iterator::operator++() {
  std::error_code ec; increment(ec);
  if (ec) throw fs::filesystem_error("directory_iterator increment", ec);
  return *this;
}
std::string vp_weakly_canonical(const std::string& path) {
  const std::string absolute = vp_u8(vp_io_path(path));
  std::string parent = absolute;
  for (;;) {
    if (char* raw = vp_realpath(parent.c_str())) {
      std::string result(raw); std::free(raw);
      if (parent.size() != absolute.size()) {
        auto suffix = absolute.substr(parent.size());
        if (!result.empty() && result.back() == '/' && !suffix.empty() && suffix.front() == '/') suffix.erase(0, 1);
        result += suffix;
      }
      return result;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
      throw std::system_error(error, std::system_category(), "WRITER_PATH_INVALID");
    const auto next = vp_path(parent).parent_path(); const auto shorter = vp_u8(next);
    if (shorter.empty() || shorter == parent) throw std::runtime_error("WRITER_PATH_INVALID");
    parent = shorter;
  }
}
#endif
