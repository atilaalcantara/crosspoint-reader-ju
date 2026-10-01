#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../HighlightEntry.h"

namespace HighlightStore {

constexpr uint8_t FORMAT_VERSION = 2;
constexpr uint8_t LEGACY_FORMAT_VERSION = 1;
constexpr uint16_t MAX_HIGHLIGHTS_PER_BOOK = 64;
constexpr uint16_t MAX_BOOK_PATH_BYTES = 1024;

// Stable FNV-1a based sidecar name; the original path is also stored in the
// file and checked on load so a hash collision cannot mix two books.
std::string getFilePath(const std::string& bookPath);

// Missing file is a successful empty collection. On malformed/incompatible
// data, returns false and leaves `entries` untouched.
bool load(const std::string& bookPath, std::vector<HighlightEntry>& entries);

// Replaces the sidecar using .tmp/.bak staging. Records are validated before
// any on-card file is changed.
bool save(const std::string& bookPath, const std::vector<HighlightEntry>& entries);

// Enumerate validated, nonempty sidecars without scanning the book library.
// Returns false if any sidecar is unreadable; callers can still show the valid paths.
bool listBookPaths(std::vector<std::string>& paths);

}  // namespace HighlightStore
