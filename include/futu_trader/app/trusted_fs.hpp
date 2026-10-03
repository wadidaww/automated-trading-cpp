#pragma once

#include <cstddef>
#include <string>

#include "futu_trader/core/result.hpp"

namespace futu_trader::app {

/**
 * The state directory: created owner-only (whatever the umask); an existing one must be a directory
 * we own that group/others cannot write.
 */
Result<bool> prepareStateDir(const std::string& dir);

/**
 * Reads a file we make a safety decision from (the promotion log): a regular file, not a symlink,
 * owned by us, not group/world writable, at most `maxBytes`. Check and read share one descriptor,
 * so the file cannot be swapped in between. Guards against mistakes and other users, not against
 * root.
 */
Result<std::string> readTrustedText(const std::string& path, std::size_t maxBytes);

}  // namespace futu_trader::app
