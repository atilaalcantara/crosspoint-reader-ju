#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "StudyPackage.h"

namespace Study {

// A sidecar is optional. No filenames are retained beyond the current card.
class MediaIndex {
 public:
  static constexpr uint32_t MAX_RECORDS = MAX_CARDS * 8;
  static constexpr uint8_t MAX_IMAGES_PER_FACE = 4;
  using Names = std::array<std::string, MAX_IMAGES_PER_FACE>;

  bool open(RandomAccess& source, uint32_t packageCrc);
  bool imagesFor(RandomAccess& source, uint64_t cardId, Names& front, Names& back);
  void reset();
  static bool safeFilename(const uint8_t* bytes, uint16_t length);
  static std::string directoryFor(const std::string& packagePath);

 private:
  uint32_t crc_ = 0;
  uint32_t count_ = 0;
  uint32_t nextRecord_ = 0;
  uint32_t nextOffset_ = 20;
  uint64_t lastCardId_ = 0;
  uint64_t previousId_ = 0;
  uint8_t previousFace_ = 0;
  uint8_t previousCount_ = 0;
  bool ready_ = false;
};

}  // namespace Study
