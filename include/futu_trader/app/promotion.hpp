#pragma once

#include <string>

#include "futu_trader/core/result.hpp"

namespace futu_trader::app {

/**
 * Records one SIMULATE day in the promotion log the live gate reads ("YYYY-MM-DD clean|dirty").
 *
 * One line per date, so a second session on the same day MERGES: the day stays clean only if every
 * session was clean (a dirty session can never be overwritten by a later clean one). The file is
 * rewritten atomically (temp file + fsync + rename) with mode 0600. An existing log that does not
 * parse is left untouched and an error is returned: guessing would invalidate evidence.
 *
 * This is a record of what the process observed, not a defence against someone with write access
 * to the file (see LiveGate's note).
 */
Result<bool> recordPromotionDay(const std::string& path, const std::string& date, bool clean);

}  // namespace futu_trader::app
