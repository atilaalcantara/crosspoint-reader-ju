#pragma once

#include "StudyPackage.h"
#include "StudyState.h"

namespace Study {

enum class SessionPhase : uint8_t { Main = 0, Again = 1, Done = 2, Cram = 3 };
enum class CardFace : uint8_t { Front = 0, Back = 1 };
enum class CheckpointSource : uint8_t { None, Canonical, Temporary, Backup, Damaged };

struct SessionCheckpoint {
  uint32_t packageCrc = 0;
  uint32_t cardCount = 0;
  uint32_t deckId = 0;
  SessionPhase phase = SessionPhase::Main;
  uint32_t nextIndex = 0;
  uint32_t currentIndex = 0xFFFFFFFFu;
  uint64_t currentCardId = 0;
  CardFace face = CardFace::Front;
};

// Deterministic local intervals; this is not FSRS or Anki scheduling.
bool applyRating(CardState& state, Rating rating, int32_t today, bool inAgainPass);
bool cardStateEligible(const CardState& state, SessionPhase phase, int32_t today);

using StateLookup = bool (*)(void* context, uint32_t index, uint64_t cardId, CardState& state, Error& error);
int32_t findNextCard(const Package& package, RandomAccess& packageFile, uint32_t deckId, bool includeDescendants, SessionPhase phase,
                     uint32_t startIndex, int32_t today, StateLookup loadState, void* context, Error& error);

inline constexpr uint32_t CHECKPOINT_BYTES = 48;
bool encodeCheckpoint(const SessionCheckpoint& checkpoint, uint8_t* bytes, uint32_t length);
bool decodeCheckpoint(const uint8_t* bytes, uint32_t length, uint32_t expectedPackageCrc, uint32_t expectedCardCount,
                      SessionCheckpoint& checkpoint);
CheckpointSource chooseCheckpointSource(bool canonicalExists, bool canonicalValid, bool backupValid,
                                        bool temporaryValid, bool damagedValid);

}  // namespace Study
