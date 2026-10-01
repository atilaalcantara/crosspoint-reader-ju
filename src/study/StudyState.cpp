#include "StudyState.h"

#include <array>
#include <cstring>
#include <memory>
#include <new>

namespace Study {
namespace {

constexpr uint8_t STATE_MAGIC[8] = {'C', 'P', 'S', 'S', 'T', 'A', 'T', 0};
constexpr uint32_t CRC_POLYNOMIAL = 0xEDB88320u;

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (CRC_POLYNOMIAL & (0u - (crc & 1u)));
  }
  return ~crc;
}

void put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}
void put32(uint8_t* p, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
void put64(uint8_t* p, uint64_t value) {
  put32(p, static_cast<uint32_t>(value));
  put32(p + 4, static_cast<uint32_t>(value >> 32));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | (static_cast<uint64_t>(get32(p + 4)) << 32); }

std::array<uint8_t, STATE_SLOT_BYTES> encode(const CardState& state, const uint32_t generation) {
  std::array<uint8_t, STATE_SLOT_BYTES> bytes{};
  put32(bytes.data(), generation);
  put64(bytes.data() + 4, state.cardId);
  put32(bytes.data() + 12, static_cast<uint32_t>(state.dueDay));
  put32(bytes.data() + 16, state.intervalDays);
  put32(bytes.data() + 20, static_cast<uint32_t>(state.lastReviewDay));
  put32(bytes.data() + 24, state.repetitions);
  put32(bytes.data() + 28, state.lapses);
  bytes[32] = static_cast<uint8_t>(state.status);
  bytes[33] = static_cast<uint8_t>(state.lastRating);
  put32(bytes.data() + 36, crc32(bytes.data(), 36));
  return bytes;
}

bool isZeroRecord(const std::array<uint8_t, STATE_RECORD_BYTES>& bytes) {
  for (const uint8_t byte : bytes) {
    if (byte != 0) return false;
  }
  return true;
}

bool decode(const uint8_t* bytes, const uint64_t expectedCardId, CardState& state, uint32_t& generation) {
  bool zero = true;
  for (uint32_t i = 0; i < STATE_SLOT_BYTES; ++i) zero = zero && bytes[i] == 0;
  if (zero) {
    state = CardState{};
    state.cardId = expectedCardId;
    generation = 0;
    return true;
  }
  if (get32(bytes + 36) != crc32(bytes, 36) || get64(bytes + 4) != expectedCardId ||
      bytes[32] > static_cast<uint8_t>(CardStatus::Suspended) || bytes[33] > static_cast<uint8_t>(Rating::Easy) ||
      bytes[34] != 0 || bytes[35] != 0) {
    return false;
  }
  generation = get32(bytes);
  state.cardId = expectedCardId;
  state.dueDay = static_cast<int32_t>(get32(bytes + 12));
  state.intervalDays = get32(bytes + 16);
  state.lastReviewDay = static_cast<int32_t>(get32(bytes + 20));
  state.repetitions = get32(bytes + 24);
  state.lapses = get32(bytes + 28);
  state.status = static_cast<CardStatus>(bytes[32]);
  state.lastRating = static_cast<Rating>(bytes[33]);
  return true;
}

}  // namespace

bool StateStore::initialize(WritableRandomAccess& file, uint32_t packageCrc, uint32_t cardCount, Error& error) {
  error = Error::None;
  if (cardCount > MAX_CARDS) {
    error = Error::LimitExceeded;
    return false;
  }
  std::array<uint8_t, STATE_HEADER_BYTES> header{};
  std::memcpy(header.data(), STATE_MAGIC, sizeof(STATE_MAGIC));
  put16(header.data() + 8, FORMAT_VERSION);
  put16(header.data() + 10, STATE_HEADER_BYTES);
  put32(header.data() + 12, packageCrc);
  put32(header.data() + 16, cardCount);
  put32(header.data() + 20, STATE_RECORD_BYTES);
  put32(header.data() + 24, 1);  // generation
  put32(header.data() + 28, crc32(header.data(), 28));
  if (!file.writeAt(0, header.data(), header.size())) {
    error = Error::Io;
    return false;
  }
  constexpr uint32_t kInitChunkBytes = 4080;  // 51 records; multiple of the 80-byte record stride.
  std::unique_ptr<uint8_t[]> zeroRecords(new (std::nothrow) uint8_t[kInitChunkBytes]{});
  if (!zeroRecords) {
    error = Error::OutOfMemory;
    return false;
  }
  uint32_t offset = STATE_HEADER_BYTES;
  uint32_t remaining = cardCount;
  while (remaining > 0) {
    const uint32_t maxBatch = kInitChunkBytes / STATE_RECORD_BYTES;
    const uint32_t batch = remaining < maxBatch ? remaining : maxBatch;
    const size_t length = static_cast<size_t>(batch) * STATE_RECORD_BYTES;
    if (!file.writeAt(offset, zeroRecords.get(), length)) {
      error = Error::Io;
      return false;
    }
    offset += static_cast<uint32_t>(length);
    remaining -= batch;
  }
  if (!file.flush()) {
    error = Error::Io;
    return false;
  }
  cardCount_ = cardCount;
  packageCrc_ = packageCrc;
  opened_ = true;
  return true;
}

bool StateStore::open(RandomAccess& file, uint32_t expectedPackageCrc, uint32_t expectedCardCount, Error& error) {
  error = Error::None;
  opened_ = false;
  std::array<uint8_t, STATE_HEADER_BYTES> header{};
  const uint64_t expectedSize = STATE_HEADER_BYTES + static_cast<uint64_t>(expectedCardCount) * STATE_RECORD_BYTES;
  if (expectedCardCount > MAX_CARDS || file.size() != expectedSize || !file.readAt(0, header.data(), header.size())) {
    error = Error::InvalidHeader;
    return false;
  }
  if (std::memcmp(header.data(), STATE_MAGIC, sizeof(STATE_MAGIC)) != 0 || get16(header.data() + 8) != FORMAT_VERSION ||
      get16(header.data() + 10) != STATE_HEADER_BYTES || get32(header.data() + 20) != STATE_RECORD_BYTES ||
      get32(header.data() + 28) != crc32(header.data(), 28)) {
    error = Error::InvalidHeader;
    return false;
  }
  if (get32(header.data() + 12) != expectedPackageCrc || get32(header.data() + 16) != expectedCardCount) {
    error = Error::InvalidHeader;
    return false;
  }
  packageCrc_ = expectedPackageCrc;
  cardCount_ = expectedCardCount;
  opened_ = true;
  return true;
}

bool StateStore::load(RandomAccess& file, uint32_t cardIndex, uint64_t expectedCardId, CardState& out, Error& error) const {
  error = Error::None;
  if (!opened_ || cardIndex >= cardCount_ || expectedCardId == 0) {
    error = Error::NotFound;
    return false;
  }
  std::array<uint8_t, STATE_RECORD_BYTES> bytes{};
  const uint32_t offset = STATE_HEADER_BYTES + cardIndex * STATE_RECORD_BYTES;
  if (!file.readAt(offset, bytes.data(), bytes.size())) {
    error = Error::Io;
    return false;
  }
  if (isZeroRecord(bytes)) {
    CardState fresh;
    fresh.cardId = expectedCardId;
    out = fresh;
    return true;
  }
  CardState first;
  CardState second;
  uint32_t firstGeneration = 0;
  uint32_t secondGeneration = 0;
  const bool firstValid = decode(bytes.data(), expectedCardId, first, firstGeneration);
  const bool secondValid = decode(bytes.data() + STATE_SLOT_BYTES, expectedCardId, second, secondGeneration);
  for (uint32_t slot = 0; slot < 2; ++slot) {
    const uint8_t* record = bytes.data() + slot * STATE_SLOT_BYTES;
    bool zero = true;
    for (uint32_t i = 0; i < STATE_SLOT_BYTES; ++i) zero = zero && record[i] == 0;
    if (!zero && get32(record + 36) == crc32(record, 36) && get64(record + 4) != expectedCardId) {
      error = Error::InvalidCard;
      return false;
    }
  }
  if (!firstValid && !secondValid) {
    error = (isZeroRecord(bytes)) ? Error::InvalidCard : Error::InvalidChecksum;
    return false;
  }
  out = !secondValid || (firstValid && firstGeneration > secondGeneration) ? first : second;
  return true;
}

bool StateStore::save(WritableRandomAccess& file, uint32_t cardIndex, const CardState& state, Error& error) const {
  error = Error::None;
  if (!opened_ || cardIndex >= cardCount_ || state.cardId == 0 ||
      static_cast<uint8_t>(state.status) > static_cast<uint8_t>(CardStatus::Suspended) ||
      static_cast<uint8_t>(state.lastRating) > static_cast<uint8_t>(Rating::Easy)) {
    error = Error::InvalidCard;
    return false;
  }
  std::array<uint8_t, STATE_RECORD_BYTES> previous{};
  const uint32_t recordOffset = STATE_HEADER_BYTES + cardIndex * STATE_RECORD_BYTES;
  if (!file.readAt(recordOffset, previous.data(), previous.size())) {
    error = Error::Io;
    return false;
  }
  CardState firstState, secondState;
  uint32_t firstGeneration = 0, secondGeneration = 0;
  const bool firstValid = decode(previous.data(), state.cardId, firstState, firstGeneration);
  const bool secondValid = decode(previous.data() + STATE_SLOT_BYTES, state.cardId, secondState, secondGeneration);
  const uint32_t activeGeneration = !secondValid || (firstValid && firstGeneration > secondGeneration)
                                        ? firstGeneration
                                        : secondGeneration;
  if (activeGeneration == UINT32_MAX) {
    error = Error::LimitExceeded;
    return false;
  }
  const uint32_t targetSlot = !firstValid ? 0 : (!secondValid ? 1 : (firstGeneration <= secondGeneration ? 0 : 1));
  const auto bytes = encode(state, activeGeneration + 1);
  const uint32_t offset = recordOffset + targetSlot * STATE_SLOT_BYTES;
  if (!file.writeAt(offset, bytes.data(), bytes.size()) || !file.flush()) {
    error = Error::Io;
    return false;
  }
  return true;
}

}  // namespace Study
