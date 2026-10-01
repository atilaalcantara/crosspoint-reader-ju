#pragma once

#include "StudyPackage.h"

namespace Study {

inline constexpr uint32_t STATE_HEADER_BYTES = 32;
inline constexpr uint32_t STATE_SLOT_BYTES = 40;
inline constexpr uint32_t STATE_RECORD_BYTES = STATE_SLOT_BYTES * 2;

enum class CardStatus : uint8_t { New = 0, Learning = 1, Review = 2, Suspended = 3 };
enum class Rating : uint8_t { None = 0, Again = 1, Hard = 2, Good = 3, Easy = 4 };

struct CardState {
  uint64_t cardId = 0;
  int32_t dueDay = 0;
  uint32_t intervalDays = 0;
  int32_t lastReviewDay = -1;
  uint32_t repetitions = 0;
  uint32_t lapses = 0;
  CardStatus status = CardStatus::New;
  Rating lastRating = Rating::None;
};

class StateStore {
 public:
  bool initialize(WritableRandomAccess& file, uint32_t packageCrc, uint32_t cardCount, Error& error);
  bool open(RandomAccess& file, uint32_t expectedPackageCrc, uint32_t expectedCardCount, Error& error);
  bool load(RandomAccess& file, uint32_t cardIndex, uint64_t expectedCardId, CardState& out, Error& error) const;
  bool save(WritableRandomAccess& file, uint32_t cardIndex, const CardState& state, Error& error) const;
  uint32_t cardCount() const { return cardCount_; }
  uint32_t packageCrc() const { return packageCrc_; }

 private:
  uint32_t cardCount_ = 0;
  uint32_t packageCrc_ = 0;
  bool opened_ = false;
};

}  // namespace Study
