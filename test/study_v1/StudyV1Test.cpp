#include <gtest/gtest.h>
#include <study/StudyPackage.h>
#include <study/StudyScheduler.h>
#include <study/StudyState.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "../support/StudyFixtureBuilder.h"

namespace {

StudyMemoryFile fixture() {
  StudyMemoryFile file;
  file.bytes = makeStudyFixture({{1, 0, "Veterinária"}, {2, 1, "Virologia"}, {3, 2, "Retrovírus"}, {4, 1, "Clínica"}},
                                {{101, 3, "O que é HIV?", "Vírus da imunodeficiência humana."},
                                 {102, 2, "Café\ncom leite", "café ☕"}, {103, 4, "", "Resposta vazia no front aceita."}});
  return file;
}

struct StateTable {
  std::array<Study::CardState, 3> cards{};
};

bool lookupState(void* context, const uint32_t index, const uint64_t cardId, Study::CardState& state,
                 Study::Error& error) {
  auto& table = *static_cast<StateTable*>(context);
  if (index >= table.cards.size() || table.cards[index].cardId != cardId) {
    error = Study::Error::InvalidCard;
    return false;
  }
  state = table.cards[index];
  return true;
}

}  // namespace

TEST(StudyPackageTest, LoadsUtf8CardsByOffsetAndBuildsNestedDeckCatalog) {
  auto file = fixture();
  Study::Package package;
  Study::Error error;
  ASSERT_TRUE(package.open(file, error)) << Study::errorName(error);
  EXPECT_EQ(package.deckCount(), 4u);
  EXPECT_EQ(package.cardCount(), 3u);
  ASSERT_NE(package.deck(1), nullptr);
  EXPECT_EQ(package.deck(1)->name, "Veterinária");
  EXPECT_EQ(package.deck(1)->subtreeCardCount, 3u);
  EXPECT_EQ(package.deck(2)->subtreeCardCount, 2u);
  EXPECT_TRUE(package.cardBelongsToDeck(0, 1, true));
  EXPECT_FALSE(package.cardBelongsToDeck(0, 4, true));

  Study::Card card;
  ASSERT_TRUE(package.loadCard(file, 1, card, error)) << Study::errorName(error);
  EXPECT_EQ(card.id, 102u);
  EXPECT_EQ(card.deckId, 2u);
  EXPECT_EQ(card.front, "Café\ncom leite");
  EXPECT_EQ(card.back, "café ☕");
  EXPECT_EQ(package.indexRamBytes(), 3u * 8u);
}

TEST(StudyPackageTest, RejectsTruncatedBadVersionAndChecksumWithoutPartialCatalog) {
  auto valid = fixture();
  Study::Package package;
  Study::Error error;
  ASSERT_TRUE(package.open(valid, error));

  auto truncated = valid;
  truncated.bytes.pop_back();
  EXPECT_FALSE(package.open(truncated, error));
  EXPECT_EQ(error, Study::Error::InvalidHeader);
  EXPECT_EQ(package.cardCount(), 0u);

  auto wrongVersion = fixture();
  wrongVersion.bytes[8] = 2;
  EXPECT_FALSE(package.open(wrongVersion, error));
  EXPECT_EQ(error, Study::Error::UnsupportedVersion);

  auto corrupt = fixture();
  corrupt.bytes.back() ^= 0x20;
  EXPECT_FALSE(package.open(corrupt, error));
  EXPECT_EQ(error, Study::Error::InvalidChecksum);
}

TEST(StudyCatalogTest, InvalidPackageDoesNotHideOtherValidFiles) {
  auto goodA = fixture();
  StudyMemoryFile bad;
  bad.bytes = {'n', 'o', 't', ' ', 'a', ' ', 'd', 'e', 'c', 'k'};
  auto goodB = fixture();
  Study::Catalog catalog;
  EXPECT_TRUE(catalog.addFile("/study/a.cps", goodA));
  EXPECT_FALSE(catalog.addFile("/study/b.cps", bad));
  EXPECT_TRUE(catalog.addFile("/study/c.cps", goodB));
  EXPECT_EQ(catalog.files().size(), 3u);
  EXPECT_EQ(catalog.validPackageCount(), 2u);
  EXPECT_EQ(catalog.files()[1].error, Study::Error::Io);
  EXPECT_EQ(catalog.files()[2].package.cardCount(), 3u);
}

TEST(StudyPackageTest, RejectsInvalidUtf8DuplicateIdsAndMissingParents) {
  Study::Error error;
  Study::Package package;
  auto badUtf8 = makeStudyFixture({{1, 0, "Deck"}}, {{1, 1, "valid", "valid"}});
  badUtf8.back() = 0xFF;
  studySet32(badUtf8, 36, studyCrc32(badUtf8.data() + Study::HEADER_BYTES, badUtf8.size() - Study::HEADER_BYTES));
  StudyMemoryFile invalidUtf8;
  invalidUtf8.bytes = badUtf8;
  EXPECT_FALSE(package.open(invalidUtf8, error));
  EXPECT_EQ(error, Study::Error::InvalidUtf8);

  StudyMemoryFile duplicate;
  duplicate.bytes = makeStudyFixture({{1, 0, "Deck"}}, {{8, 1, "a", "b"}, {8, 1, "c", "d"}});
  EXPECT_FALSE(package.open(duplicate, error));
  EXPECT_EQ(error, Study::Error::InvalidCard);

  StudyMemoryFile missingParent;
  missingParent.bytes = makeStudyFixture({{1, 99, "Orphan"}}, {});
  EXPECT_FALSE(package.open(missingParent, error));
  EXPECT_EQ(error, Study::Error::InvalidDeck);
}

TEST(StudyPackageTest, EnforcesFaceAndDeckNameLimitsAndUtf8Validity) {
  Study::Package package;
  Study::Error error;
  StudyMemoryFile tooLong;
  tooLong.bytes = makeStudyFixture({{1, 0, "Deck"}}, {{1, 1, "x", std::string(Study::MAX_FACE_BYTES + 1, 'b')}});
  EXPECT_FALSE(package.open(tooLong, error));
  EXPECT_EQ(error, Study::Error::LimitExceeded);

  StudyMemoryFile badDeckName;
  badDeckName.bytes = makeStudyFixture({{1, 0, std::string(Study::MAX_DECK_NAME_BYTES + 1, 'n')}}, {});
  EXPECT_FALSE(package.open(badDeckName, error));
  EXPECT_EQ(error, Study::Error::LimitExceeded);

  const std::array<uint8_t, 2> overlong = {0xC0, 0xAF};
  EXPECT_FALSE(Study::isValidUtf8(overlong.data(), overlong.size()));
}

TEST(StudyStateTest, InitializesAndRoundTripsStateBoundToPackageAndCardIdentity) {
  constexpr uint32_t packageCrc = 0x1234ABCD;
  StudyMemoryFile stateFile;
  Study::StateStore store;
  Study::Error error;
  ASSERT_TRUE(store.initialize(stateFile, packageCrc, 2, error));
  ASSERT_TRUE(store.open(stateFile, packageCrc, 2, error));

  Study::CardState state;
  ASSERT_TRUE(store.load(stateFile, 0, 77, state, error));
  EXPECT_EQ(state.cardId, 77u);
  EXPECT_EQ(state.status, Study::CardStatus::New);
  state.status = Study::CardStatus::Review;
  state.lastRating = Study::Rating::Good;
  state.dueDay = 21000;
  state.intervalDays = 6;
  state.lastReviewDay = 20994;
  state.repetitions = 4;
  ASSERT_TRUE(store.save(stateFile, 0, state, error));

  Study::StateStore reopened;
  ASSERT_TRUE(reopened.open(stateFile, packageCrc, 2, error));
  Study::CardState loaded;
  ASSERT_TRUE(reopened.load(stateFile, 0, 77, loaded, error));
  EXPECT_EQ(loaded.status, Study::CardStatus::Review);
  EXPECT_EQ(loaded.lastRating, Study::Rating::Good);
  EXPECT_EQ(loaded.dueDay, 21000);
  EXPECT_EQ(loaded.intervalDays, 6u);
  EXPECT_EQ(loaded.repetitions, 4u);
  EXPECT_FALSE(reopened.load(stateFile, 0, 78, loaded, error));
  EXPECT_EQ(error, Study::Error::InvalidCard);
}

TEST(StudyStateTest, RejectsWrongPackageCorruptionAndTruncatedState) {
  StudyMemoryFile stateFile;
  Study::StateStore store;
  Study::Error error;
  ASSERT_TRUE(store.initialize(stateFile, 42, 1, error));
  Study::StateStore wrongPackage;
  EXPECT_FALSE(wrongPackage.open(stateFile, 43, 1, error));

  ASSERT_TRUE(store.save(stateFile, 0, {9, 1, 2, 0, 1, 0, Study::CardStatus::Review, Study::Rating::Good}, error));
  ASSERT_TRUE(store.save(stateFile, 0, {9, 5, 4, 1, 2, 0, Study::CardStatus::Review, Study::Rating::Easy}, error));
  stateFile.bytes[Study::STATE_HEADER_BYTES + Study::STATE_SLOT_BYTES + 10] ^= 0x01;
  Study::CardState loaded;
  ASSERT_TRUE(store.load(stateFile, 0, 9, loaded, error));
  EXPECT_EQ(loaded.intervalDays, 2u);
  EXPECT_EQ(loaded.lastRating, Study::Rating::Good);

  stateFile.bytes.pop_back();
  EXPECT_FALSE(wrongPackage.open(stateFile, 42, 1, error));
}

TEST(StudyStateTest, RefusesGenerationWrapWithoutOverwritingLastValidSlot) {
  StudyMemoryFile file;
  Study::StateStore store;
  Study::Error error;
  ASSERT_TRUE(store.initialize(file, 81, 1, error));
  ASSERT_TRUE(store.open(file, 81, 1, error));
  std::array<uint8_t, Study::STATE_SLOT_BYTES> slot{};
  for (uint8_t i = 0; i < 4; ++i) slot[i] = static_cast<uint8_t>(UINT32_MAX >> (i * 8));
  const uint64_t cardId = 88;
  for (uint8_t i = 0; i < 8; ++i) slot[4 + i] = static_cast<uint8_t>(cardId >> (i * 8));
  const uint32_t crc = studyCrc32(slot.data(), 36);
  for (uint8_t i = 0; i < 4; ++i) slot[36 + i] = static_cast<uint8_t>(crc >> (i * 8));
  ASSERT_TRUE(file.writeAt(Study::STATE_HEADER_BYTES, slot.data(), slot.size()));
  const auto before = file.bytes;
  Study::CardState state{cardId, 1, 1, 0, 2, 0, Study::CardStatus::Review, Study::Rating::Good};
  EXPECT_FALSE(store.save(file, 0, state, error));
  EXPECT_EQ(error, Study::Error::LimitExceeded);
  EXPECT_EQ(file.bytes, before);
}

TEST(StudySchedulerTest, RatingsProduceDocumentedIntervalsAndAgainRetryIsBounded) {
  Study::CardState state;
  state.cardId = 5;
  ASSERT_TRUE(Study::applyRating(state, Study::Rating::Hard, 20000, false));
  EXPECT_EQ(state.intervalDays, 1u);
  EXPECT_EQ(state.dueDay, 20001);
  ASSERT_TRUE(Study::applyRating(state, Study::Rating::Good, 20001, false));
  EXPECT_EQ(state.intervalDays, 2u);
  EXPECT_EQ(state.dueDay, 20003);
  ASSERT_TRUE(Study::applyRating(state, Study::Rating::Easy, 20003, false));
  EXPECT_EQ(state.intervalDays, 6u);
  ASSERT_TRUE(Study::applyRating(state, Study::Rating::Again, 20009, false));
  EXPECT_EQ(state.status, Study::CardStatus::Learning);
  EXPECT_EQ(state.dueDay, 20009);
  ASSERT_TRUE(Study::applyRating(state, Study::Rating::Again, 20009, true));
  EXPECT_EQ(state.status, Study::CardStatus::Review);
  EXPECT_EQ(state.intervalDays, 1u);
  EXPECT_EQ(state.dueDay, 20010);
  EXPECT_EQ(state.lapses, 2u);
}

TEST(StudySchedulerTest, CheckpointEligibilityRejectsCardsAlreadyScheduledForLater) {
  Study::CardState state;
  state.cardId = 31;
  EXPECT_TRUE(Study::cardStateEligible(state, Study::SessionPhase::Main, 20000));
  state.status = Study::CardStatus::Review;
  state.dueDay = 20000;
  EXPECT_TRUE(Study::cardStateEligible(state, Study::SessionPhase::Main, 20000));
  state.dueDay = 20001;
  EXPECT_FALSE(Study::cardStateEligible(state, Study::SessionPhase::Main, 20000));
  state.status = Study::CardStatus::Learning;
  state.dueDay = 20000;
  state.lastRating = Study::Rating::Again;
  EXPECT_FALSE(Study::cardStateEligible(state, Study::SessionPhase::Main, 20000));
  EXPECT_TRUE(Study::cardStateEligible(state, Study::SessionPhase::Again, 20000));
  state.lastRating = Study::Rating::Good;
  EXPECT_FALSE(Study::cardStateEligible(state, Study::SessionPhase::Again, 20000));
}

TEST(StudySchedulerTest, CramAdmitsEveryCardRegardlessOfSchedule) {
  auto file = fixture();
  Study::Package package;
  Study::Error error;
  ASSERT_TRUE(package.open(file, error));
  StateTable states;
  // Card 0: review scheduled far in the future; card 1: learning; card 2: new.
  // Main skips the future card, but Cram visits every card in index order.
  states.cards[0] = {101, 22000, 5, 21000, 3, 0, Study::CardStatus::Review, Study::Rating::Good};
  states.cards[1] = {102, 21000, 0, 21000, 2, 1, Study::CardStatus::Learning, Study::Rating::Again};
  states.cards[2] = {103, 0, 0, -1, 0, 0, Study::CardStatus::New, Study::Rating::None};
  EXPECT_EQ(Study::findNextCard(package, file, 1, true, Study::SessionPhase::Main, 0, 21000, lookupState, &states,
                                error),
            2);
  EXPECT_EQ(Study::findNextCard(package, file, 1, true, Study::SessionPhase::Cram, 0, 21000, lookupState, &states,
                                error),
            0);
  EXPECT_EQ(Study::findNextCard(package, file, 1, true, Study::SessionPhase::Cram, 1, 21000, lookupState, &states,
                                error),
            1);
  Study::SessionCheckpoint checkpoint;
  checkpoint.packageCrc = 0x12345678;
  checkpoint.cardCount = 3;
  checkpoint.deckId = 1;
  checkpoint.phase = Study::SessionPhase::Cram;
  checkpoint.nextIndex = 1;
  checkpoint.currentIndex = 0;
  checkpoint.currentCardId = 101;
  std::array<uint8_t, Study::CHECKPOINT_BYTES> bytes{};
  ASSERT_TRUE(Study::encodeCheckpoint(checkpoint, bytes.data(), bytes.size()));
  Study::SessionCheckpoint loaded;
  ASSERT_TRUE(Study::decodeCheckpoint(bytes.data(), bytes.size(), checkpoint.packageCrc, checkpoint.cardCount, loaded));
  EXPECT_EQ(loaded.phase, Study::SessionPhase::Cram);
}

TEST(StudySchedulerTest, CheckpointRoundTripsAndRejectsChangedPackageOrBounds) {
  Study::SessionCheckpoint checkpoint;
  checkpoint.packageCrc = 0xAABBCCDD;
  checkpoint.cardCount = 12;
  checkpoint.deckId = 7;
  checkpoint.phase = Study::SessionPhase::Again;
  checkpoint.nextIndex = 4;
  checkpoint.currentIndex = 3;
  checkpoint.currentCardId = 99;
  checkpoint.face = Study::CardFace::Back;
  std::array<uint8_t, Study::CHECKPOINT_BYTES> bytes{};
  ASSERT_TRUE(Study::encodeCheckpoint(checkpoint, bytes.data(), bytes.size()));
  Study::SessionCheckpoint loaded;
  ASSERT_TRUE(Study::decodeCheckpoint(bytes.data(), bytes.size(), checkpoint.packageCrc, checkpoint.cardCount, loaded));
  EXPECT_EQ(loaded.deckId, 7u);
  EXPECT_EQ(loaded.currentCardId, 99u);
  EXPECT_EQ(loaded.face, Study::CardFace::Back);
  EXPECT_FALSE(Study::decodeCheckpoint(bytes.data(), bytes.size(), 1, checkpoint.cardCount, loaded));
  EXPECT_FALSE(Study::decodeCheckpoint(bytes.data(), bytes.size(), checkpoint.packageCrc, 2, loaded));
  bytes[24] = 20;  // next index exceeds package count
  const uint32_t crc = studyCrc32(bytes.data(), 44);
  for (uint8_t i = 0; i < 4; ++i) bytes[44 + i] = static_cast<uint8_t>(crc >> (i * 8));
  EXPECT_FALSE(Study::decodeCheckpoint(bytes.data(), bytes.size(), checkpoint.packageCrc, checkpoint.cardCount, loaded));
}

TEST(StudySchedulerTest, CheckpointRecoveryPrefersCanonicalThenBackupThenOrphanTemporary) {
  using Study::CheckpointSource;
  EXPECT_EQ(Study::chooseCheckpointSource(true, true, true, true, true), CheckpointSource::Canonical);
  EXPECT_EQ(Study::chooseCheckpointSource(true, false, true, true, false), CheckpointSource::Temporary);
  EXPECT_EQ(Study::chooseCheckpointSource(true, false, true, false, true), CheckpointSource::Backup);
  EXPECT_EQ(Study::chooseCheckpointSource(false, false, false, true, false), CheckpointSource::Temporary);
  EXPECT_EQ(Study::chooseCheckpointSource(true, false, false, false, true), CheckpointSource::Damaged);
  EXPECT_EQ(Study::chooseCheckpointSource(true, false, false, false, false), CheckpointSource::None);
  EXPECT_EQ(Study::chooseCheckpointSource(false, false, false, false, false), CheckpointSource::None);
}

TEST(StudySchedulerTest, DeterministicMainThenAgainPassUsesSubtreeAndAscendingIndex) {
  auto file = fixture();
  Study::Package package;
  Study::Error error;
  ASSERT_TRUE(package.open(file, error));
  StateTable states;
  states.cards[0] = {101, 21010, 3, 21007, 2, 0, Study::CardStatus::Review, Study::Rating::Good};
  states.cards[1] = {102, 21000, 0, 21000, 2, 1, Study::CardStatus::Learning, Study::Rating::Again};
  states.cards[2] = {103, 20999, 1, 20998, 1, 0, Study::CardStatus::Review, Study::Rating::Hard};

  EXPECT_EQ(Study::findNextCard(package, file, 1, true, Study::SessionPhase::Main, 0, 21000, lookupState, &states,
                                error),
            2);
  EXPECT_EQ(Study::findNextCard(package, file, 1, true, Study::SessionPhase::Again, 0, 21000, lookupState, &states,
                                error),
            1);
  EXPECT_EQ(Study::findNextCard(package, file, 3, false, Study::SessionPhase::Main, 0, 21000, lookupState, &states,
                                error),
            -1);
}

TEST(StudyPackageTest, OffsetIndexMemoryAndCatalogBuildScaleLinearly) {
  constexpr uint32_t count = 10000;
  std::vector<StudyFixtureCard> cards;
  cards.reserve(count);
  for (uint32_t i = 1; i <= count; ++i) cards.push_back({i, 1, "front", "back"});
  StudyMemoryFile file;
  file.bytes = makeStudyFixture({{1, 0, "Generated"}}, std::move(cards));
  Study::Package package;
  Study::Error error;
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(package.open(file, error)) << Study::errorName(error);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(package.indexRamBytes(), count * 8u);
  EXPECT_EQ(package.indexRamBytes() / 1000, 80u);  // 8 KB / 1,000 cards
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}
