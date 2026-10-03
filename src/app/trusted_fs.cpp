#include "futu_trader/app/trusted_fs.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <filesystem>

namespace futu_trader::app {

Result<bool> prepareStateDir(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const bool existed = fs::exists(dir, ec);
  fs::create_directories(dir, ec);
  if (ec) {
    return Error{ErrorCode::kInvalidArg, "cannot create state dir " + dir + ": " + ec.message()};
  }
  if (!existed) {
    ::chmod(dir.c_str(), 0700);
  }
  struct stat info {};
  if (::stat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
    return Error{ErrorCode::kInvalidArg, "state dir is not a directory: " + dir};
  }
  if (info.st_uid != ::geteuid()) {
    return Error{ErrorCode::kInvalidArg, "state dir is not owned by the running user: " + dir};
  }
  if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return Error{ErrorCode::kInvalidArg, "state dir must not be group/world writable: " + dir};
  }
  return true;
}

Result<std::string> readTrustedText(const std::string& path, std::size_t maxBytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return Error{ErrorCode::kInvalidArg, "cannot open (missing, or a symlink): " + path};
  }
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error{ErrorCode::kInvalidArg, "not a regular file: " + path};
  }
  if (info.st_uid != ::geteuid() || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return Error{ErrorCode::kInvalidArg,
                 "must be owned by us and not group/world writable: " + path};
  }
  if (static_cast<std::size_t>(info.st_size) > maxBytes) {
    return Error{ErrorCode::kInvalidArg, "file too large: " + path};
  }
  std::string text;
  std::array<char, 4096> buf{};
  while (text.size() <= maxBytes) {  // bounded even if the file grows after fstat
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) {
      break;
    }
    text.append(buf.data(), static_cast<std::size_t>(n));
  }
  if (text.size() > maxBytes) {
    return Error{ErrorCode::kInvalidArg, "file too large: " + path};
  }
  return text;
}

}  // namespace futu_trader::app
