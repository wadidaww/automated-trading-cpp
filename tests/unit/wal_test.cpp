#include "futu_trader/infra/wal.hpp"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

using namespace futu_trader;
using namespace futu_trader::infra;

namespace {

struct TempFile {
  explicit TempFile(const std::string& name)
      : path((std::filesystem::temp_directory_path() / name).string()) {
    std::filesystem::remove(path);
  }
  ~TempFile() { std::filesystem::remove(path); }
  std::string path;
};

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST(Wal, DurableAppendsRoundTripInOrder) {
  TempFile file("futu_wal_roundtrip.wal");
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok()) << wal.error().message;
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(wal.value()->appendDurable("record-" + std::to_string(i)).ok());
    }
  }
  const auto read = readWal(file.path);
  ASSERT_EQ(read.status, WalStatus::kOk);
  ASSERT_EQ(read.records.size(), 200U);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(read.records[static_cast<std::size_t>(i)], "record-" + std::to_string(i));
  }
}

TEST(Wal, ADurableRecordIsReadableTheMomentAppendReturns) {
  // Write-ahead means: when appendDurable() returns, a crash right now must not lose the record.
  TempFile file("futu_wal_durable.wal");
  auto wal = Wal::open({file.path});
  ASSERT_TRUE(wal.ok());
  ASSERT_TRUE(wal.value()->appendDurable("intent: buy 100 00700").ok());
  const auto read = readWal(file.path);  // the log is still open: simulates reading after a crash
  ASSERT_EQ(read.records.size(), 1U);
  EXPECT_EQ(read.records[0], "intent: buy 100 00700");
  EXPECT_GE(wal.value()->stats().syncs.load(), 1U);
}

TEST(Wal, ReopeningAppendsWithoutDuplicatingTheHeader) {
  TempFile file("futu_wal_reopen.wal");
  for (int session = 0; session < 3; ++session) {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(wal.value()->appendDurable("session-" + std::to_string(session)).ok());
  }
  const auto read = readWal(file.path);
  ASSERT_EQ(read.status, WalStatus::kOk);
  EXPECT_EQ(read.records, (std::vector<std::string>{"session-0", "session-1", "session-2"}));
}

TEST(Wal, TheFileIsCreatedOwnerOnly) {
  TempFile file("futu_wal_mode.wal");
  auto wal = Wal::open({file.path});
  ASSERT_TRUE(wal.ok());
  struct stat info {};
  ASSERT_EQ(::stat(file.path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600U);  // order intents are sensitive: not group/world readable
}

TEST(Wal, EveryPossibleTornTailIsDetectedAndEarlierRecordsSurvive) {
  TempFile file("futu_wal_torn.wal");
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(wal.value()->appendDurable("payload-" + std::to_string(i)).ok());
    }
  }
  const std::string full = slurp(file.path);
  TempFile cut("futu_wal_torn_cut.wal");
  std::size_t completeCases = 0;
  for (std::size_t length = 0; length <= full.size(); ++length) {
    spit(cut.path, full.substr(0, length));
    const auto read = readWal(cut.path);  // must never crash, hang or over-allocate
    EXPECT_LE(read.records.size(), 5U);
    for (std::size_t i = 0; i < read.records.size(); ++i) {
      ASSERT_EQ(read.records[i], "payload-" + std::to_string(i));  // a clean prefix, never garbage
    }
    if (read.status == WalStatus::kOk && length >= 8) {
      ++completeCases;
    }
  }
  EXPECT_EQ(completeCases, 6U);  // exactly the 6 record boundaries (after header + each record)
  EXPECT_EQ(readWal(file.path).status, WalStatus::kOk);
}

TEST(Wal, ACorruptByteStopsReadingAtThatRecordAndKeepsEverythingBeforeIt) {
  TempFile file("futu_wal_corrupt.wal");
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    for (int i = 0; i < 4; ++i) {
      ASSERT_TRUE(wal.value()->appendDurable("record-number-" + std::to_string(i)).ok());
    }
  }
  std::string bytes = slurp(file.path);
  const std::size_t secondRecordPayload = 8 + (4 + 15 + 4) + 4 + 3;  // inside record #1's payload
  bytes[secondRecordPayload] = static_cast<char>(bytes[secondRecordPayload] ^ 0x20);
  spit(file.path, bytes);
  const auto read = readWal(file.path);
  EXPECT_EQ(read.status, WalStatus::kBadChecksum);
  EXPECT_EQ(read.records,
            (std::vector<std::string>{"record-number-0"}));  // nothing after is trusted
}

TEST(Wal, ABadHeaderVersionOrLengthIsRefusedWithoutAllocating) {
  TempFile file("futu_wal_bad.wal");
  spit(file.path, "this is not a wal");
  EXPECT_EQ(readWal(file.path).status, WalStatus::kBadHeader);
  spit(file.path, std::string("FWAL") + std::string("\x07\x00\x00\x00", 4));
  EXPECT_EQ(readWal(file.path).status, WalStatus::kBadVersion);
  spit(file.path, std::string("FWAL\x01\x00\x00\x00", 8) + std::string("\xFF\xFF\xFF\x7F", 4));
  EXPECT_EQ(readWal(file.path).status, WalStatus::kBadRecord);  // a ~2 GiB length is not believed
  EXPECT_EQ(readWal("/nonexistent/futu.wal").status, WalStatus::kUnreadable);
}

TEST(Wal, ManyThreadsKeepTheirOwnOrderAndLoseNothing) {
  TempFile file("futu_wal_threads.wal");
  constexpr int kThreads = 8;
  constexpr int kEach = 300;
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < kEach; ++i) {
          const std::string payload = std::to_string(t) + ":" + std::to_string(i);
          if (i % 10 == 0) {
            EXPECT_TRUE(wal.value()->appendDurable(payload).ok());
          } else {
            EXPECT_TRUE(wal.value()->appendAsync(payload));
          }
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
  }
  const auto read = readWal(file.path);
  ASSERT_EQ(read.status, WalStatus::kOk);
  EXPECT_EQ(read.records.size(), static_cast<std::size_t>(kThreads * kEach));
  std::map<int, int> next;
  for (const auto& record : read.records) {
    const auto colon = record.find(':');
    const int thread = std::stoi(record.substr(0, colon));
    const int seq = std::stoi(record.substr(colon + 1));
    ASSERT_EQ(seq, next[thread]++) << "thread " << thread << " reordered";
  }
}

TEST(Wal, FlushAndDestructionDrainTheAsyncQueue) {
  TempFile file("futu_wal_drain.wal");
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    for (int i = 0; i < 500; ++i) {
      ASSERT_TRUE(wal.value()->appendAsync("a" + std::to_string(i)));
    }
    ASSERT_TRUE(wal.value()->flush().ok());
    EXPECT_EQ(readWal(file.path).records.size(), 500U);  // flush made them visible
    for (int i = 0; i < 300; ++i) {
      ASSERT_TRUE(wal.value()->appendAsync("b" + std::to_string(i)));
    }
  }  // destructor: the writer must drain before exiting
  EXPECT_EQ(readWal(file.path).records.size(), 800U);
}

TEST(Wal, AFullAsyncQueueDropsAndCountsInsteadOfBlockingOrCorrupting) {
  TempFile file("futu_wal_overflow.wal");
  std::size_t accepted = 0;
  {
    WalConfig cfg{file.path};
    cfg.asyncQueueLimit = 4;
    cfg.flushInterval = std::chrono::milliseconds(200);  // slow writer: the queue fills
    auto wal = Wal::open(cfg);
    ASSERT_TRUE(wal.ok());
    for (int i = 0; i < 20'000; ++i) {
      accepted += wal.value()->appendAsync("r" + std::to_string(i)) ? 1U : 0U;
    }
    EXPECT_GT(wal.value()->stats().droppedAsync.load(), 0U);
    EXPECT_EQ(accepted + wal.value()->stats().droppedAsync.load(), 20'000U);
  }
  const auto read = readWal(file.path);
  EXPECT_EQ(read.status, WalStatus::kOk);
  EXPECT_EQ(read.records.size(), accepted);  // exactly what was accepted, intact
}

TEST(Wal, OversizeRecordsAreRefused) {
  TempFile file("futu_wal_big.wal");
  auto wal = Wal::open({file.path});
  ASSERT_TRUE(wal.ok());
  const std::string big(kMaxWalRecordBytes + 1, 'x');
  EXPECT_FALSE(wal.value()->appendDurable(big).ok());
  EXPECT_FALSE(wal.value()->appendAsync(big));
  EXPECT_TRUE(wal.value()->appendDurable("still fine").ok());
}

TEST(Wal, AnUnwritablePathOrFullDiskIsAnErrorNotASilentSuccess) {
  EXPECT_FALSE(Wal::open({"/nonexistent-dir/futu.wal"}).ok());
  // /dev/full accepts the open and fails every write with ENOSPC.
  EXPECT_FALSE(Wal::open({"/dev/full"}).ok());
}

TEST(Wal, ADiskWriteFailureFailsClosedForEverySubsequentAppend) {
  // Provoke a real write error: a tiny RLIMIT_FSIZE makes write(2) fail with EFBIG once the file
  // would grow past it. Done in a forked child so the limit cannot affect the test process.
  TempFile file("futu_wal_fsize.wal");
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::signal(SIGXFSZ, SIG_IGN);  // otherwise exceeding the limit kills the process
    rlimit limit{256, 256};
    ::setrlimit(RLIMIT_FSIZE, &limit);
    auto wal = Wal::open({file.path});
    if (!wal.ok()) {
      ::_exit(10);
    }
    bool sawFailure = false;
    for (int i = 0; i < 200 && !sawFailure; ++i) {
      sawFailure = !wal.value()->appendDurable(std::string(40, 'x')).ok();
    }
    const bool refusesNow = !wal.value()->appendDurable("after").ok();
    const bool refusesAsync = !wal.value()->appendAsync("after");
    ::_exit(sawFailure && refusesNow && refusesAsync && wal.value()->failed() ? 0 : 11);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << "10: open failed, 11: failure not detected / not sticky";
}

TEST(Wal, ReopeningAfterATornTailCutsItOffSoLaterRecordsAreReachable) {
  TempFile file("futu_wal_repair.wal");
  std::string twoRecords;
  std::string threeRecords;
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(wal.value()->appendDurable("before-crash-1").ok());
    ASSERT_TRUE(wal.value()->appendDurable("before-crash-2").ok());
    twoRecords = slurp(file.path);
    ASSERT_TRUE(wal.value()->appendDurable("was-being-written-at-crash").ok());
    threeRecords = slurp(file.path);
  }
  // A crash can stop the third write after ANY number of bytes: try every one of them.
  for (std::size_t cut = twoRecords.size(); cut < threeRecords.size(); ++cut) {
    spit(file.path, threeRecords.substr(0, cut));
    {
      auto wal = Wal::open({file.path});
      ASSERT_TRUE(wal.ok()) << "cut=" << cut;
      ASSERT_TRUE(wal.value()->appendDurable("after-restart").ok());
    }
    const auto read = readWal(file.path);
    EXPECT_EQ(read.status, WalStatus::kOk) << "cut=" << cut;
    EXPECT_EQ(read.records,
              (std::vector<std::string>{"before-crash-1", "before-crash-2", "after-restart"}))
        << "cut=" << cut;
  }
}

TEST(Wal, ATornHeaderIsRepairedAsAnEmptyLog) {
  TempFile file("futu_wal_halfheader.wal");
  spit(file.path, "FWA");
  auto wal = Wal::open({file.path});
  ASSERT_TRUE(wal.ok());
  ASSERT_TRUE(wal.value()->appendDurable("x").ok());
  wal.value().reset();
  EXPECT_EQ(readWal(file.path).records, (std::vector<std::string>{"x"}));
}

TEST(Wal, ACorruptLogIsRefusedNotAppendedToOrOverwritten) {
  TempFile file("futu_wal_refuse.wal");
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(wal.value()->appendDurable("intent-1").ok());
    ASSERT_TRUE(wal.value()->appendDurable("intent-2").ok());
  }
  std::string bytes = slurp(file.path);
  bytes[12] = static_cast<char>(bytes[12] ^ 0x01);  // inside the first record's payload
  spit(file.path, bytes);
  EXPECT_FALSE(Wal::open({file.path}).ok());
  EXPECT_EQ(slurp(file.path), bytes);  // untouched: evidence preserved

  spit(file.path, "this is definitely not a wal file");
  EXPECT_FALSE(Wal::open({file.path}).ok());
}

TEST(Wal, MidFileCorruptionThatLooksLikeATornTailIsRefusedNotTruncated) {
  TempFile file("futu_wal_midcorrupt.wal");
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(wal.value()->appendDurable("intent-number-" + std::to_string(i)).ok());
    }
  }
  std::string bytes = slurp(file.path);
  // Record 1's length field now claims 5000 bytes: it "runs past the end", exactly like a torn
  // tail.
  const std::size_t secondRecord = 8 + (4 + 15 + 4);
  bytes[secondRecord] = static_cast<char>(5000 & 0xFF);
  bytes[secondRecord + 1] = static_cast<char>(5000 >> 8);
  spit(file.path, bytes);
  EXPECT_EQ(readWal(file.path).status, WalStatus::kTruncated);  // the ambiguity is real
  EXPECT_FALSE(Wal::open({file.path}).ok());
  EXPECT_EQ(slurp(file.path), bytes);  // records 2-4 were NOT destroyed
}

TEST(Wal, ARealTornTailIsSavedAsideBeforeItIsCut) {
  TempFile file("futu_wal_savetorn.wal");
  std::string two;
  std::string three;
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
    ASSERT_TRUE(wal.value()->appendDurable("a").ok());
    two = slurp(file.path);
    ASSERT_TRUE(wal.value()->appendDurable("b-being-written").ok());
    three = slurp(file.path);
  }
  spit(file.path, three.substr(0, two.size() + 6));
  {
    auto wal = Wal::open({file.path});
    ASSERT_TRUE(wal.ok());
  }
  const std::string saved = file.path + ".torn-" + std::to_string(two.size());
  EXPECT_EQ(slurp(saved), three.substr(two.size(), 6));
  std::filesystem::remove(saved);
}

TEST(Wal, ASecondOpenOfTheSameLogIsRefusedWhileTheFirstIsAlive) {
  TempFile file("futu_wal_lock.wal");
  auto first = Wal::open({file.path});
  ASSERT_TRUE(first.ok());
  const auto second = Wal::open({file.path});
  ASSERT_FALSE(second.ok());
  EXPECT_NE(second.error().message.find("locked"), std::string::npos);
  ASSERT_TRUE(first.value()->appendDurable("still works").ok());
  first.value().reset();  // releasing the first makes the log available again
  EXPECT_TRUE(Wal::open({file.path}).ok());
}
