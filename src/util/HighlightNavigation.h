#pragma once

#include <string>
#include <string_view>
#include <cstdint>

#include "../HighlightEntry.h"

namespace HighlightNavigation {

inline bool comesBefore(const HighlightEntry& left, const HighlightEntry& right) {
  if (left.spineIndex != right.spineIndex) return left.spineIndex < right.spineIndex;
  if (left.startOffset != right.startOffset) return left.startOffset < right.startOffset;
  if (left.endOffset != right.endOffset) return left.endOffset < right.endOffset;
  return left.id < right.id;
}

// Bound list rows by Unicode characters, not bytes, and never split a UTF-8
// codepoint. The complete quote remains in the store and is used for navigation.
inline std::string quotePreview(const std::string_view quote, const size_t maxCodepoints = 88) {
  if (quote.empty()) return {};
  if (maxCodepoints == 0) return "…";
  size_t byteCount = 0;
  size_t count = 0;
  while (byteCount < quote.size() && count < maxCodepoints) {
    ++byteCount;
    while (byteCount < quote.size() && (static_cast<uint8_t>(quote[byteCount]) & 0xC0) == 0x80) ++byteCount;
    ++count;
  }
  const bool truncated = byteCount < quote.size();
  std::string preview(quote.data(), byteCount);
  if (truncated) preview += "…";
  return preview;
}

}  // namespace HighlightNavigation
