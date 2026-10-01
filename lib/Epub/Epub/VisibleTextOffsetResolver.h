#pragma once

#include <cstdint>
#include <optional>

// Streaming resolver shared by the in-memory and SD-backed Section page LUTs.
// It chooses the last page whose start is <= the requested visible offset;
// preferFirstAtOffset preserves the earliest page when zero-width pages share
// the exact same start offset.
class VisibleTextOffsetResolver {
 public:
  explicit VisibleTextOffsetResolver(const uint32_t offset, const bool preferFirstAtOffset = false)
      : offset_(offset), preferFirstAtOffset_(preferFirstAtOffset) {}

  // Consume one page start. Returns true when iteration can stop.
  bool consume(const uint16_t page, const uint32_t pageStart) {
    if (!result_) result_ = page;
    if (preferFirstAtOffset_ && pageStart == offset_) {
      result_ = page;
      return true;
    }
    if (pageStart > offset_) return true;
    result_ = page;
    return false;
  }

  std::optional<uint16_t> result() const { return result_; }

 private:
  uint32_t offset_;
  bool preferFirstAtOffset_;
  std::optional<uint16_t> result_;
};
