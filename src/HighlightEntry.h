#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// A persisted EPUB text range. Offsets are zero-based Unicode codepoints in
// the visible text of one spine's <body>, not UTF-8 bytes or screen geometry.
struct HighlightEntry {
  static constexpr uint16_t MAX_QUOTE_BYTES = 512;
  // The list keeps all notes resident alongside the bounded highlight set;
  // 512 bytes caps worst-case note storage at 32 KiB per book.
  static constexpr uint16_t MAX_NOTE_BYTES = 512;

  uint32_t id = 0;
  uint16_t spineIndex = 0;
  uint32_t startOffset = 0;
  uint32_t endOffset = 0;  // half-open [startOffset, endOffset)
  std::string quote;
  std::string note;  // Empty means no note.

  static bool isValidNoteUtf8(const std::string_view value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
    size_t i = 0;
    while (i < value.size()) {
      const uint8_t lead = bytes[i++];
      if (lead == 0) return false;
      if (lead <= 0x7F) continue;

      uint8_t continuationCount = 0;
      uint8_t secondMin = 0x80;
      uint8_t secondMax = 0xBF;
      if (lead >= 0xC2 && lead <= 0xDF) {
        continuationCount = 1;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        continuationCount = 2;
        if (lead == 0xE0) secondMin = 0xA0;
        if (lead == 0xED) secondMax = 0x9F;  // Exclude UTF-16 surrogate values.
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        continuationCount = 3;
        if (lead == 0xF0) secondMin = 0x90;
        if (lead == 0xF4) secondMax = 0x8F;  // Unicode ends at U+10FFFF.
      } else {
        return false;
      }
      if (i + continuationCount > value.size()) return false;
      const uint8_t second = bytes[i++];
      if (second < secondMin || second > secondMax) return false;
      for (uint8_t remaining = continuationCount - 1; remaining > 0; --remaining) {
        const uint8_t continuation = bytes[i++];
        if (continuation < 0x80 || continuation > 0xBF) return false;
      }
    }
    return true;
  }

  bool isValid() const {
    return id != 0 && startOffset < endOffset && !quote.empty() && quote.size() <= MAX_QUOTE_BYTES &&
           note.size() <= MAX_NOTE_BYTES && isValidNoteUtf8(note);
  }

  bool overlaps(const uint16_t spine, const uint32_t wordStart, const uint32_t wordEnd) const {
    return spineIndex == spine && wordStart < wordEnd && wordStart < endOffset && wordEnd > startOffset;
  }
};
