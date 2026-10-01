#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace HighlightSelection {

// Touch geometry is transient and is never written to HighlightEntry.
struct WordBox {
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  uint32_t visibleStart = 0;
  uint32_t visibleEnd = 0;
};

struct TextRange {
  uint32_t start = 0;
  uint32_t end = 0;
};

// State for the part of a touch contact owned by text selection. The caller
// decides when a stationary contact qualifies as a long press; after that,
// moves and the release endpoint belong to the selection rather than paging.
class DragGesture {
 public:
  void pointerDown(const size_t word) {
    anchor_ = focus_ = word;
    tracking_ = true;
    active_ = false;
  }

  bool activateLongPress() {
    if (!tracking_) return false;
    active_ = true;
    return true;
  }

  bool moveTo(const size_t word) {
    if (!tracking_ || !active_) return false;
    focus_ = word;
    return true;
  }

  bool release(const std::optional<size_t> finalWord = std::nullopt) {
    if (!tracking_) return false;
    // Releasing outside a word keeps the last valid focus reached while held.
    if (active_ && finalWord) focus_ = *finalWord;
    tracking_ = false;
    return active_;
  }

  void cancel() {
    tracking_ = false;
    active_ = false;
    anchor_ = focus_ = 0;
  }

  bool tracking() const { return tracking_; }
  bool active() const { return active_; }
  size_t anchor() const { return anchor_; }
  size_t focus() const { return focus_; }

 private:
  size_t anchor_ = 0;
  size_t focus_ = 0;
  bool tracking_ = false;
  bool active_ = false;
};

inline std::optional<size_t> hitTest(const std::vector<WordBox>& words, const int x, const int y, const int slop = 5) {
  size_t best = words.size();
  int64_t bestDistance = INT64_MAX;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& word = words[i];
    if (word.width <= 0 || word.height <= 0 || word.visibleStart >= word.visibleEnd || x < word.x - slop ||
        x >= word.x + word.width + slop || y < word.y - slop || y >= word.y + word.height + slop) {
      continue;
    }
    const int64_t dx = static_cast<int64_t>(2 * x - (2 * word.x + word.width));
    const int64_t dy = static_cast<int64_t>(2 * y - (2 * word.y + word.height));
    const int64_t distance = dx * dx + dy * dy;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best == words.size() ? std::nullopt : std::optional<size_t>(best);
}

inline std::optional<TextRange> normalize(const std::vector<WordBox>& words, const size_t anchor, const size_t focus) {
  if (anchor >= words.size() || focus >= words.size()) return std::nullopt;
  const uint32_t start = std::min(words[anchor].visibleStart, words[focus].visibleStart);
  const uint32_t end = std::max(words[anchor].visibleEnd, words[focus].visibleEnd);
  if (start >= end) return std::nullopt;
  return TextRange{start, end};
}

inline bool overlaps(const TextRange& range, const uint32_t start, const uint32_t end) {
  return start < end && start < range.end && end > range.start;
}

inline bool quoteNeedsSeparator(const uint32_t previousEnd, const uint32_t nextStart, const bool newVisualLine,
                                const char previousByte) {
  if (nextStart > previousEnd) return true;
  // Compact EPUB markup can put adjacent paragraphs at contiguous source
  // offsets. A visual line break supplies the missing word boundary unless
  // the preceding fragment visibly carries a hyphen.
  return nextStart == previousEnd && newVisualLine && previousByte != '-';
}

}  // namespace HighlightSelection
