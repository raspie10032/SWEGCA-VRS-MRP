#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include "vrs_handle_cache.h"

bool vrs_file_identity(int fd, VrsFileIdentity& result) {
#ifdef _WIN32
  FILE_ID_INFO info{};
  if (!GetFileInformationByHandleEx(reinterpret_cast<HANDLE>(intptr_t(fd)), FileIdInfo, &info, sizeof info)) return false;
  result.volume = info.VolumeSerialNumber;
  std::memcpy(&result.low, info.FileId.Identifier, 8);
  std::memcpy(&result.high, info.FileId.Identifier + 8, 8);
#else
  struct stat info{};
  if (fstat(fd, &info)) return false;
  result.volume = uint64_t(info.st_dev); result.low = uint64_t(info.st_ino); result.high = 0;
#endif
  return true;
}
