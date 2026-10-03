#include <gtest/gtest.h>

#include <random>
#include <sstream>

#include "futu_trader/data/csv.hpp"
#include "futu_trader/data/event_log.hpp"
#include "futu_trader/data/validate.hpp"

using namespace futu_trader;
using namespace futu_trader::data;

namespace {
QuoteEvent quote(std::int64_t ts, const std::string& sym = "00700", Money bid = 350'000,
                 Money ask = 350'200) {
  return {ts, sym, bid, ask, 350'000, 1000, 2000};
}
}  // namespace

// --- CRC ----------------------------------------------------------------------------------------

TEST(Crc32, KnownVector) {
  const std::string text = "123456789";
  EXPECT_EQ(crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), 0xCBF43926U);
  EXPECT_EQ(crc32(nullptr, 0), 0U);
}

// --- Event log ----------------------------------------------------------------------------------

TEST(EventLog, RoundTripsManyRandomQuotes) {
  std::mt19937 rng(11);
  std::vector<QuoteEvent> written;
  std::stringstream stream;
  EventLogWriter writer(stream);
  for (int i = 0; i < 1000; ++i) {
    QuoteEvent q = quote(1'000'000'000LL * i, i % 2 ? "00700" : "09988",
                         static_cast<Money>(rng() % 1'000'000 + 1), 0);
    q.ask = q.bid + static_cast<Money>(rng() % 500);
    q.bidSize = rng() % 10'000;
    q.askSize = rng() % 10'000;
    ASSERT_TRUE(writer.writeQuote(q));
    written.push_back(q);
  }
  const auto result = readEventLog(stream);
  ASSERT_EQ(result.status, LogStatus::kOk);
  ASSERT_EQ(result.quotes.size(), written.size());
  for (std::size_t i = 0; i < written.size(); ++i) {
    ASSERT_EQ(result.quotes[i], written[i]) << i;
  }
}

TEST(EventLog, EmptyLogIsValid) {
  std::stringstream stream;
  EventLogWriter writer(stream);
  const auto result = readEventLog(stream);
  EXPECT_EQ(result.status, LogStatus::kOk);
  EXPECT_TRUE(result.quotes.empty());
}

TEST(EventLog, TornTailIsDetectedAndEarlierRecordsSurvive) {
  std::stringstream full;
  EventLogWriter writer(full);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(writer.writeQuote(quote(i * 1000)));
  }
  const std::string bytes = full.str();
  // Cut in the middle of the last record, as a crash mid-write would.
  std::stringstream torn(bytes.substr(0, bytes.size() - 7));
  const auto result = readEventLog(torn);
  EXPECT_EQ(result.status, LogStatus::kTruncated);
  EXPECT_EQ(result.quotes.size(), 4U);
}

TEST(EventLog, EveryPossibleTruncationIsHandledWithoutCrashing) {
  std::stringstream full;
  EventLogWriter writer(full);
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(writer.writeQuote(quote(i * 1000)));
  }
  const std::string bytes = full.str();
  for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
    std::stringstream partial(bytes.substr(0, cut));
    const auto result = readEventLog(partial);
    EXPECT_LE(result.quotes.size(), 3U);
    if (cut == bytes.size()) {
      EXPECT_EQ(result.status, LogStatus::kOk);
    }
  }
}

TEST(EventLog, FlippedByteIsACheckSumFailureNotSilentBadData) {
  std::stringstream full;
  EventLogWriter writer(full);
  ASSERT_TRUE(writer.writeQuote(quote(1000)));
  ASSERT_TRUE(writer.writeQuote(quote(2000)));
  std::string bytes = full.str();
  bytes[8 + 4 + 12] = static_cast<char>(bytes[8 + 4 + 12] ^ 0x40);  // inside the first payload
  std::stringstream corrupt(bytes);
  const auto result = readEventLog(corrupt);
  EXPECT_EQ(result.status, LogStatus::kBadChecksum);
  EXPECT_TRUE(result.quotes.empty());  // nothing after (or including) the bad record is trusted
}

TEST(EventLog, RejectsWrongMagicVersionAndHostileLengths) {
  {
    std::stringstream notALog("this is definitely not an event log");
    EXPECT_EQ(readEventLog(notALog).status, LogStatus::kBadHeader);
  }
  {
    std::stringstream empty;
    EXPECT_EQ(readEventLog(empty).status, LogStatus::kBadHeader);
  }
  {
    std::string header = std::string("FTEL") + std::string("\x09\x00\x00\x00", 4);
    std::stringstream future(header);
    EXPECT_EQ(readEventLog(future).status, LogStatus::kBadVersion);
  }
  {
    std::stringstream stream;
    EventLogWriter writer(stream);
    std::string bytes = stream.str();
    bytes += std::string("\xFF\xFF\xFF\x7F", 4);  // claims a ~2 GiB record
    std::stringstream hostile(bytes);
    EXPECT_EQ(readEventLog(hostile).status, LogStatus::kBadRecord);
  }
}

TEST(EventLog, WriterRefusesOverlongSymbols) {
  std::stringstream stream;
  EventLogWriter writer(stream);
  QuoteEvent q = quote(1);
  q.symbol = std::string(300, 'X');
  EXPECT_FALSE(writer.writeQuote(q));
}

// --- CSV ----------------------------------------------------------------------------------------

TEST(Csv, ParsePriceMillsIsExact) {
  EXPECT_EQ(parsePriceMills("350.2").value(), 350'200);
  EXPECT_EQ(parsePriceMills("0.001").value(), 1);
  EXPECT_EQ(parsePriceMills("350").value(), 350'000);
  EXPECT_EQ(parsePriceMills("9995.000").value(), 9'995'000);
  EXPECT_EQ(parsePriceMills("0.25").value(), 250);
}

TEST(Csv, ParsePriceMillsRejectsEverythingQuestionable) {
  for (const char* bad : {"", ".", "350.", ".5", "-1", "+1", "1e3", "1.2345", "abc", "1 2",
                          "350.2.1", " 350", "99999999999999999999"}) {
    EXPECT_FALSE(parsePriceMills(bad).ok()) << bad;
  }
}

TEST(Csv, ParsesQuotes) {
  const auto parsed = parseQuotesCsv(
      "ts_ms,symbol,bid,ask,last,bid_size,ask_size\n"
      "1000,00700,350.0,350.2,350.0,1000,2000\r\n"
      "\n"
      "2000,00700,350.2,350.4,350.2,500,600\n");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  ASSERT_EQ(parsed.value().size(), 2U);
  EXPECT_EQ(parsed.value()[0].tsNs, 1'000'000'000);
  EXPECT_EQ(parsed.value()[0].bid, 350'000);
  EXPECT_EQ(parsed.value()[1].ask, 350'400);
  EXPECT_EQ(parsed.value()[1].askSize, 600);
}

TEST(Csv, BadRowsFailTheWholeFileWithALineNumber) {
  const std::string header = "ts_ms,symbol,bid,ask,last,bid_size,ask_size\n";
  const auto crossed = parseQuotesCsv(header + "1000,00700,350.4,350.2,350.0,1,1\n");
  ASSERT_FALSE(crossed.ok());
  EXPECT_NE(crossed.error().message.find("line 2"), std::string::npos);
  EXPECT_FALSE(parseQuotesCsv(header + "1000,00700,350.0,350.2,350.0,1\n").ok());  // 6 fields
  EXPECT_FALSE(parseQuotesCsv(header + "x,00700,350.0,350.2,350.0,1,1\n").ok());
  EXPECT_FALSE(parseQuotesCsv(header + "1000,,350.0,350.2,350.0,1,1\n").ok());
  EXPECT_FALSE(parseQuotesCsv(header + "1000,00700,0,350.2,350.0,1,1\n").ok());
  const auto backwards =
      parseQuotesCsv(header + "2000,00700,350,351,350,1,1\n1000,00700,350,351,350,1,1\n");
  ASSERT_FALSE(backwards.ok());
  EXPECT_NE(backwards.error().message.find("backwards"), std::string::npos);
  EXPECT_FALSE(parseQuotesCsv("wrong,header\n").ok());
  EXPECT_FALSE(parseQuotesCsv("").ok());
}

// --- Validation ---------------------------------------------------------------------------------

TEST(ValidateQuotes, AcceptsSaneDataIncludingEqualTimestamps) {
  EXPECT_TRUE(validateQuotes({}).ok());
  EXPECT_TRUE(validateQuotes({quote(1), quote(2), quote(2), quote(3)}).ok());
}

TEST(ValidateQuotes, RejectsEveryKindOfNonsenseWithTheEventIndex) {
  auto backwards = std::vector<QuoteEvent>{quote(5), quote(3)};
  const auto result = validateQuotes(backwards);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message.find("event 1"), std::string::npos);
  QuoteEvent crossed = quote(1);
  crossed.bid = 351'000;
  EXPECT_FALSE(validateQuotes({crossed}).ok());
  QuoteEvent zeroBid = quote(1);
  zeroBid.bid = 0;
  EXPECT_FALSE(validateQuotes({zeroBid}).ok());
  QuoteEvent negativeSize = quote(1);
  negativeSize.askSize = -1;
  EXPECT_FALSE(validateQuotes({negativeSize}).ok());
  QuoteEvent noSymbol = quote(1);
  noSymbol.symbol.clear();
  EXPECT_FALSE(validateQuotes({noSymbol}).ok());
  EXPECT_FALSE(validateQuotes({quote(-1)}).ok());
}

TEST(ValidateQuotes, ALogWithValidChecksumsCanStillHoldNonsense) {
  // The checksum only proves the bytes are intact. A buggy recorder writing crossed quotes and
  // backwards timestamps produces a log that reads back "ok"; validation is what catches it.
  std::stringstream stream;
  EventLogWriter writer(stream);
  QuoteEvent crossed = quote(9);
  crossed.bid = 360'000;
  ASSERT_TRUE(writer.writeQuote(quote(10)));
  ASSERT_TRUE(writer.writeQuote(crossed));
  const auto loaded = readEventLog(stream);
  ASSERT_EQ(loaded.status, LogStatus::kOk);
  EXPECT_FALSE(validateQuotes(loaded.quotes).ok());
}
