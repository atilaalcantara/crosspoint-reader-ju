#include "StudyMedia.h"

#include <array>
#include <cstring>

namespace Study {
namespace {
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }
}  // namespace

void MediaIndex::reset() {
  ready_ = false;
  count_ = nextRecord_ = 0;
  nextOffset_ = 20;
  lastCardId_ = 0;
  previousId_ = 0;
  previousFace_ = previousCount_ = 0;
  crc_ = 0;
}

bool MediaIndex::safeFilename(const uint8_t* bytes, uint16_t length) {
  // SHA-256 prefix (32 lowercase hex digits), followed by the fixed PNG suffix.
  if (length != 36 || std::memcmp(bytes + 32, ".png", 4) != 0) return false;
  for (unsigned i = 0; i < 32; ++i) {
    if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f'))) return false;
  }
  return true;
}

std::string MediaIndex::directoryFor(const std::string& packagePath) {
  if (packagePath.size() < 12 || packagePath.compare(0, 7, "/study/") != 0 ||
      packagePath.find('/', 7) != std::string::npos) return {};
  const size_t suffix = packagePath.size() - 4;
  if (packagePath.compare(suffix, 4, ".cps") != 0) return {};
  return packagePath.substr(0, suffix) + ".media";
}

bool MediaIndex::open(RandomAccess& source, uint32_t packageCrc) {
  reset();
  std::array<uint8_t, 20> header{};
  const uint64_t size = source.size();
  if (size < header.size() || size > 20ull + static_cast<uint64_t>(MAX_RECORDS) * (12 + 64) ||
      !source.readAt(0, header.data(), header.size()) || std::memcmp(header.data(), "CPSMEDIA", 8) != 0 ||
      u16(header.data() + 8) != 1 || u16(header.data() + 10) != 0 || u32(header.data() + 12) != packageCrc)
    return false;
  count_ = u32(header.data() + 16);
  if (count_ > MAX_RECORDS || size < 20ull + static_cast<uint64_t>(count_) * 13 ||
      size > 20ull + static_cast<uint64_t>(count_) * 76) {
    reset();
    return false;
  }
  crc_ = packageCrc;
  ready_ = true;
  return true;
}

bool MediaIndex::imagesFor(RandomAccess& source, uint64_t cardId, Names& front, Names& back) {
  front = {};
  back = {};
  if (!ready_ || cardId == 0) return false;
  if (cardId <= lastCardId_) {
    nextRecord_ = 0;
    nextOffset_ = 20;
    lastCardId_ = 0;
    previousId_ = 0;
    previousFace_ = previousCount_ = 0;
  }
  // A card lookup never materializes the index. Rewind only when review order goes backwards.
  uint32_t record = nextRecord_;
  uint32_t offset = nextOffset_;
  uint8_t faceCounts[2] = {};
  while (record < count_) {
    std::array<uint8_t, 12> prefix{};
    if (static_cast<uint64_t>(offset) + prefix.size() > source.size() ||
        !source.readAt(offset, prefix.data(), prefix.size())) break;
    const uint64_t id = u64(prefix.data());
    const uint8_t face = prefix[8];
    const uint16_t length = u16(prefix.data() + 10);
    if (id == 0 || face > 1 || prefix[9] != 0 || length < 1 || length > 64 ||
        static_cast<uint64_t>(offset) + 12 + length > source.size() ||
        (previousId_ && (id < previousId_ || (id == previousId_ && face < previousFace_)))) break;
    if (id > cardId) {
      nextRecord_ = record;
      nextOffset_ = offset;
      lastCardId_ = cardId;
      return true;
    }
    std::array<uint8_t, 64> name{};
    if (!source.readAt(offset + 12, name.data(), length) || !safeFilename(name.data(), length)) break;
    previousCount_ = id == previousId_ && face == previousFace_ ? previousCount_ + 1 : 1;
    if (previousCount_ > MAX_IMAGES_PER_FACE) break;
    if (id == cardId) {
      auto& names = face == 0 ? front : back;
      names[faceCounts[face]++].assign(reinterpret_cast<const char*>(name.data()), length);
    }
    previousId_ = id;
    previousFace_ = face;
    offset += 12 + length;
    ++record;
  }
  if (record != count_ || offset != source.size()) {
    front = {};
    back = {};
    reset();
    return false;
  }
  nextRecord_ = record;
  nextOffset_ = offset;
  lastCardId_ = cardId;
  return true;
}

}  // namespace Study
