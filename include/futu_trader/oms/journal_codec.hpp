#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "futu_trader/core/types.hpp"
#include "futu_trader/oms/oms.hpp"

namespace futu_trader::oms {

/**
 * The intent of one order, written durably BEFORE the order leaves the process. After a crash it
 * is the only proof that an order may exist at the broker, which is what stops a restarted process
 * from sending a second copy of an order whose first reply was lost.
 */
struct DurableSubmit {
  std::int64_t tsNs{0};
  std::string clOrdId;
  std::string intentKey;
  std::string symbol;
  Side side{Side::kBuy};
  std::int64_t qty{0};
  Money priceMills{0};
};

/** Binary, versioned, length-prefixed (no delimiter ambiguity). Record kind is the first byte. */
std::string encodeSubmit(const DurableSubmit& submit);
std::string encodeJournal(const JournalEntry& entry);

/** nullopt for a record that is not a well-formed submit (wrong kind, truncated, trailing bytes).
 */
std::optional<DurableSubmit> decodeSubmit(std::string_view bytes);
std::optional<JournalEntry> decodeJournal(std::string_view bytes);

}  // namespace futu_trader::oms
