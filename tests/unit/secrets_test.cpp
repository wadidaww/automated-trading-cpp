#include "futu_trader/infra/secrets.hpp"

#include <gtest/gtest.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace futu_trader;
using namespace futu_trader::infra;

namespace {

struct TempDir {
  TempDir()
      : path(std::filesystem::temp_directory_path() /
             ("futu_secret_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
  std::string file(const std::string& name, const std::string& content, mode_t mode) const {
    const auto full = (path / name).string();
    {
      std::ofstream out(full, std::ios::binary);
      out << content;
    }
    ::chmod(full.c_str(), mode);
    return full;
  }
  std::filesystem::path path;
};

}  // namespace

TEST(Secret, HoldsTheValueAndIsRedactedWhenPrinted) {
  Secret s("hunter2");
  EXPECT_EQ(s.reveal(), "hunter2");
  EXPECT_STREQ(s.toString(), "<redacted>");
  EXPECT_FALSE(s.empty());
  EXPECT_TRUE(Secret().empty());
}

TEST(Secret, MovingTransfersOwnershipAndEmptiesTheSource) {
  Secret a("abc123");
  Secret b(std::move(a));
  EXPECT_TRUE(a.empty());  // NOLINT(bugprone-use-after-move): the contract under test
  EXPECT_EQ(b.reveal(), "abc123");
  Secret c;
  c = std::move(b);
  EXPECT_EQ(c.reveal(), "abc123");
  EXPECT_TRUE(b.empty());  // NOLINT(bugprone-use-after-move)
  c = std::move(c);        // self-move must not destroy the value  // NOLINT
  EXPECT_EQ(c.reveal(), "abc123");
}

TEST(SecretFile, ReadsAPrivateFileAndStripsOneTrailingNewline) {
  TempDir dir;
  const auto ok = readSecretFile(dir.file("a", "abcdef0123\n", 0600));
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(ok.value().reveal(), "abcdef0123");
  const auto crlf = readSecretFile(dir.file("b", "token\r\n", 0400));
  ASSERT_TRUE(crlf.ok());
  EXPECT_EQ(crlf.value().reveal(), "token");
  const auto twoLines = readSecretFile(dir.file("c", "token\n\n", 0600));
  ASSERT_TRUE(twoLines.ok());
  EXPECT_EQ(twoLines.value().reveal(), "token\n");  // only one newline is stripped
}

TEST(SecretFile, RefusesFilesOthersCanReadOrWrite) {
  TempDir dir;
  for (const mode_t bad : {0640, 0604, 0660, 0644, 0666, 0602}) {
    const auto result = readSecretFile(dir.file("s" + std::to_string(bad), "secret", bad));
    EXPECT_FALSE(result.ok()) << std::oct << bad;
  }
}

TEST(SecretFile, RefusesSymlinksMissingEmptyAndOversizeFiles) {
  TempDir dir;
  const auto real = dir.file("real", "secret", 0600);
  const auto link = (dir.path / "link").string();
  ASSERT_EQ(::symlink(real.c_str(), link.c_str()), 0);
  EXPECT_FALSE(readSecretFile(link).ok());
  EXPECT_FALSE(readSecretFile((dir.path / "missing").string()).ok());
  EXPECT_FALSE(readSecretFile(dir.file("empty", "", 0600)).ok());
  EXPECT_FALSE(readSecretFile(dir.file("nl", "\n", 0600)).ok());
  EXPECT_FALSE(
      readSecretFile(dir.file("big", std::string(kMaxSecretFileBytes + 1, 'x'), 0600)).ok());
  EXPECT_TRUE(readSecretFile(dir.file("max", std::string(kMaxSecretFileBytes, 'x'), 0600)).ok());
  EXPECT_FALSE(readSecretFile(dir.path.string()).ok());  // a directory
}

TEST(SecretFile, ErrorsNeverContainTheSecretValue) {
  TempDir dir;
  const auto result = readSecretFile(dir.file("leaky", "TOP-SECRET-VALUE", 0644));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().message.find("TOP-SECRET-VALUE"), std::string::npos);
}

TEST(Hardening, DisablesCoreDumpsAndDumpable) {
  // Run in a child: it changes process-wide limits that other tests should not inherit.
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto report = hardenProcess(false);
    rlimit limit{};
    ::getrlimit(RLIMIT_CORE, &limit);
    ::_exit(report.coreDumpsDisabled && report.dumpableCleared && limit.rlim_cur == 0 &&
                    ::prctl(PR_GET_DUMPABLE) == 0
                ? 0
                : 1);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(SecretFile, AFifoAtThePathIsRefusedInsteadOfHangingTheProcess) {
  TempDir dir;
  const auto fifo = (dir.path / "fifo").string();
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_FALSE(readSecretFile(fifo).ok());  // would block forever on open() without O_NONBLOCK
}
