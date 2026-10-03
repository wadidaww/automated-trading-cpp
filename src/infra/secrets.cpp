#include "futu_trader/infra/secrets.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace futu_trader::infra {
namespace {

// Not removable by the optimiser, unlike memset on a buffer that is about to be freed.
void secureZero(void* p, std::size_t n) {
  if (p != nullptr && n != 0) {
    explicit_bzero(p, n);
  }
}

}  // namespace

Secret::Secret(std::string_view value) {
  if (value.empty()) {
    return;
  }
  data_ = new char[value.size()];
  size_ = value.size();
  std::memcpy(data_, value.data(), size_);
}

Secret::~Secret() { wipe(); }

Secret::Secret(Secret&& other) noexcept : data_(other.data_), size_(other.size_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

Secret& Secret::operator=(Secret&& other) noexcept {
  if (this != &other) {
    wipe();
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

void Secret::wipe() {
  secureZero(data_, size_);
  delete[] data_;
  data_ = nullptr;
  size_ = 0;
}

Result<Secret> readSecretFile(const std::string& path) {
  // O_NOFOLLOW: a symlink (possibly planted to point at another file) is refused outright.
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return Error{ErrorCode::kInvalidArg,
                 "secret file cannot be opened (missing, unreadable or a symlink): " + path};
  }
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};

  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error{ErrorCode::kInvalidArg, "secret file is not a regular file: " + path};
  }
  if (info.st_uid != ::geteuid()) {
    return Error{ErrorCode::kInvalidArg, "secret file is not owned by the running user: " + path};
  }
  if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return Error{ErrorCode::kInvalidArg,
                 "secret file must not be accessible by group or others (chmod 600): " + path};
  }
  if (info.st_size <= 0 || static_cast<std::size_t>(info.st_size) > kMaxSecretFileBytes) {
    return Error{ErrorCode::kInvalidArg, "secret file is empty or too large: " + path};
  }

  std::array<char, kMaxSecretFileBytes + 1> storage{};
  char* const buffer = storage.data();
  std::size_t total = 0;
  while (total < storage.size()) {
    const ssize_t n = ::read(fd, buffer + total, storage.size() - total);
    if (n < 0) {
      secureZero(buffer, storage.size());
      return Error{ErrorCode::kInvalidArg, "secret file read failed: " + path};
    }
    if (n == 0) {
      break;
    }
    total += static_cast<std::size_t>(n);
  }
  if (total > kMaxSecretFileBytes) {
    secureZero(buffer, storage.size());
    return Error{ErrorCode::kInvalidArg, "secret file is too large: " + path};
  }
  if (total > 0 && buffer[total - 1] == '\n') {
    --total;
    if (total > 0 && buffer[total - 1] == '\r') {
      --total;
    }
  }
  Secret secret{std::string_view(buffer, total)};
  secureZero(buffer, storage.size());
  if (secret.empty()) {
    return Error{ErrorCode::kInvalidArg, "secret file holds no value: " + path};
  }
  return secret;
}

HardeningReport hardenProcess(bool lockMemory) {
  HardeningReport report;
  const rlimit noCore{0, 0};
  report.coreDumpsDisabled = ::setrlimit(RLIMIT_CORE, &noCore) == 0;
  report.dumpableCleared = ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0;
  if (lockMemory) {
    report.memoryLocked = ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
  }
  return report;
}

}  // namespace futu_trader::infra
