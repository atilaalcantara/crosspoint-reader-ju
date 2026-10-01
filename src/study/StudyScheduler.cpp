#include "StudyScheduler.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Study {
namespace {

constexpr uint8_t CHECKPOINT_MAGIC[8] = {'C', 'P', 'S', 'S', 'E', 'S', 'S', 0};
constexpr uint32_t CRC_POLYNOMIAL = 0xEDB88320u;

uint32_t crc32(const uint8_t* bytes, uint32_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < length; ++i) {
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
  for (uint8_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
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

int32_t dueAfter(int32_t today, uint32_t interval) {
  const int64_t due = static_cast<int64_t>(today) + interval;
  return due > INT32_MAX ? INT32_MAX : static_cast<int32_t>(due);
}

}  // namespace

bool cardStateEligible(const CardState& state, const SessionPhase phase, const int32_t today) {
  // Cram ("study again" after Done) revisits every card once in deck order;
  // ratings still apply normally, so Again answers retry in the Again pass.
  if (phase == SessionPhase::Cram) return true;
  if (phase == SessionPhase::Main) {
    return state.status == CardStatus::New || (state.status == CardStatus::Review && state.dueDay <= today);
  }
  return phase == SessionPhase::Again && state.status == CardStatus::Learning && state.dueDay <= today &&
         state.lastRating == Rating::Again;
}

bool applyRating(CardState& state, const Rating rating, const int32_t today, const bool inAgainPass) {
  if (rating == Rating::None || static_cast<uint8_t>(rating) > static_cast<uint8_t>(Rating::Easy) ||
      state.status == CardStatus::Suspended || state.cardId == 0) {
    return false;
  }
  const bool wasReviewed = state.repetitions > 0;
  if (state.repetitions < UINT32_MAX) ++state.repetitions;
  state.lastReviewDay = today;
  state.lastRating = rating;

  if (rating == Rating::Again) {
    if (wasReviewed && state.lapses < UINT32_MAX) ++state.lapses;
    if (inAgainPass) {
      state.status = CardStatus::Review;
      state.intervalDays = 1;
      state.dueDay = dueAfter(today, 1);
    } else {
      state.status = CardStatus::Learning;
      state.intervalDays = 0;
      state.dueDay = today;  // Reappears once in the retry pass.
    }
    return true;
  }

  const uint32_t previousInterval = state.intervalDays;
  uint64_t nextInterval = 1;
  switch (rating) {
    case Rating::Hard:
      nextInterval = previousInterval == 0
                         ? 1
                         : std::max<uint64_t>(static_cast<uint64_t>(previousInterval) + 1,
                                              (static_cast<uint64_t>(previousInterval) * 6 + 4) / 5);
      break;
    case Rating::Good:
      nextInterval = previousInterval == 0
                         ? 3
                         : std::max<uint64_t>(static_cast<uint64_t>(previousInterval) + 1,
                                              static_cast<uint64_t>(previousInterval) * 2);
      break;
    case Rating::Easy:
      nextInterval = previousInterval == 0
                         ? 5
                         : std::max<uint64_t>(static_cast<uint64_t>(previousInterval) + 2,
                                              static_cast<uint64_t>(previousInterval) * 3);
      break;
    case Rating::Again:
    case Rating::None: return false;
  }
  if (nextInterval > UINT32_MAX) nextInterval = UINT32_MAX;
  state.status = CardStatus::Review;
  state.intervalDays = static_cast<uint32_t>(nextInterval);
  state.dueDay = dueAfter(today, state.intervalDays);
  return true;
}

int32_t findNextCard(const Package& package, RandomAccess& packageFile, const uint32_t deckId,
                     const bool includeDescendants,
                     const SessionPhase phase, const uint32_t startIndex, const int32_t today,
                     const StateLookup loadState, void* context, Error& error) {
  error = Error::None;
  if (phase == SessionPhase::Done || !loadState || startIndex > package.cardCount() || !package.deck(deckId)) {
    error = Error::InvalidDeck;
    return -1;
  }
  for (uint32_t index = startIndex; index < package.cardCount(); ++index) {
    if (!package.cardBelongsToDeck(index, deckId, includeDescendants)) continue;
    uint64_t cardId = 0;
    if (!package.cardIdAt(packageFile, index, cardId, error)) return -1;
    CardState state;
    if (!loadState(context, index, cardId, state, error)) return -1;
    if (cardStateEligible(state, phase, today)) return static_cast<int32_t>(index);
  }
  error = Error::None;
  return -1;
}

bool encodeCheckpoint(const SessionCheckpoint& checkpoint, uint8_t* bytes, const uint32_t length) {
  if (!bytes || length != CHECKPOINT_BYTES || checkpoint.deckId == 0 ||
      static_cast<uint8_t>(checkpoint.phase) > static_cast<uint8_t>(SessionPhase::Cram) ||
      static_cast<uint8_t>(checkpoint.face) > static_cast<uint8_t>(CardFace::Back)) {
    return false;
  }
  std::memset(bytes, 0, CHECKPOINT_BYTES);
  std::memcpy(bytes, CHECKPOINT_MAGIC, sizeof(CHECKPOINT_MAGIC));
  put16(bytes + 8, 1);
  put16(bytes + 10, CHECKPOINT_BYTES);
  put32(bytes + 12, checkpoint.packageCrc);
  put32(bytes + 16, checkpoint.deckId);
  bytes[20] = static_cast<uint8_t>(checkpoint.phase);
  bytes[21] = static_cast<uint8_t>(checkpoint.face);
  put32(bytes + 24, checkpoint.nextIndex);
  put32(bytes + 28, checkpoint.currentIndex);
  put64(bytes + 32, checkpoint.currentCardId);
  put32(bytes + 40, checkpoint.cardCount);
  put32(bytes + 44, crc32(bytes, 44));
  return true;
}

bool decodeCheckpoint(const uint8_t* bytes, const uint32_t length, const uint32_t expectedPackageCrc,
                      const uint32_t expectedCardCount, SessionCheckpoint& checkpoint) {
  if (!bytes || length != CHECKPOINT_BYTES || std::memcmp(bytes, CHECKPOINT_MAGIC, sizeof(CHECKPOINT_MAGIC)) != 0 ||
      get16(bytes + 8) != 1 || get16(bytes + 10) != CHECKPOINT_BYTES || bytes[22] != 0 || bytes[23] != 0 ||
      get32(bytes + 44) != crc32(bytes, 44) || get32(bytes + 12) != expectedPackageCrc ||
      get32(bytes + 40) != expectedCardCount) {
    return false;
  }
  const auto phase = static_cast<SessionPhase>(bytes[20]);
  const auto face = static_cast<CardFace>(bytes[21]);
  const uint32_t nextIndex = get32(bytes + 24);
  const uint32_t currentIndex = get32(bytes + 28);
  const uint64_t currentCardId = get64(bytes + 32);
  if (bytes[20] > static_cast<uint8_t>(SessionPhase::Cram) || bytes[21] > static_cast<uint8_t>(CardFace::Back) ||
      get32(bytes + 16) == 0 || nextIndex > expectedCardCount ||
      (currentIndex != 0xFFFFFFFFu && currentIndex >= expectedCardCount) ||
      ((currentIndex == 0xFFFFFFFFu) != (currentCardId == 0)) ||
      (currentIndex != 0xFFFFFFFFu &&
       (phase == SessionPhase::Done || nextIndex != currentIndex + 1)) ||
      (currentIndex == 0xFFFFFFFFu && phase != SessionPhase::Done && currentCardId != 0)) {
    return false;
  }
  checkpoint.packageCrc = expectedPackageCrc;
  checkpoint.cardCount = expectedCardCount;
  checkpoint.deckId = get32(bytes + 16);
  checkpoint.phase = phase;
  checkpoint.face = face;
  checkpoint.nextIndex = nextIndex;
  checkpoint.currentIndex = currentIndex;
  checkpoint.currentCardId = currentCardId;
  return true;
}

CheckpointSource chooseCheckpointSource(const bool canonicalExists, const bool canonicalValid, const bool backupValid,
                                        const bool temporaryValid, const bool damagedValid) {
  if (canonicalValid) return CheckpointSource::Canonical;
  if (temporaryValid) return CheckpointSource::Temporary;
  if (backupValid) return CheckpointSource::Backup;
  if (damagedValid) return CheckpointSource::Damaged;
  (void)canonicalExists;
  return CheckpointSource::None;
}

}  // namespace Study
