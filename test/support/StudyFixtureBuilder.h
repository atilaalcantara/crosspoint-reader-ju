#pragma once

#include <study/StudyPackage.h>
#include <study/StudyState.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

class StudyMemoryFile final : public Study::WritableRandomAccess {
 public:
  std::vector<uint8_t> bytes;
  uint64_t size() const override { return bytes.size(); }
  bool readAt(uint32_t offset, void* destination, size_t length) override {
    if (offset > bytes.size() || length > bytes.size() - offset) return false;
    std::memcpy(destination, bytes.data() + offset, length);
    return true;
  }
  bool writeAt(uint32_t offset, const void* source, size_t length) override {
    if (offset > bytes.size() || length > bytes.size() - offset) bytes.resize(static_cast<size_t>(offset) + length);
    std::memcpy(bytes.data() + offset, source, length);
    return true;
  }
  bool flush() override { return true; }
};

struct StudyFixtureDeck {
  uint32_t id;
  uint32_t parent;
  std::string name;
};
struct StudyFixtureCard {
  uint64_t id;
  uint32_t deckId;
  std::string front;
  std::string back;
};

inline void studyPut16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}
inline void studyPut32(std::vector<uint8_t>& out, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline void studyPut64(std::vector<uint8_t>& out, uint64_t value) {
  studyPut32(out, static_cast<uint32_t>(value));
  studyPut32(out, static_cast<uint32_t>(value >> 32));
}
inline void studySet16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value);
  out[offset + 1] = static_cast<uint8_t>(value >> 8);
}
inline void studySet32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) out[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
inline uint32_t studyCrc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

inline std::vector<uint8_t> makeStudyFixture(std::vector<StudyFixtureDeck> decks, std::vector<StudyFixtureCard> cards) {
  std::sort(cards.begin(), cards.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  std::vector<uint8_t> bytes(Study::HEADER_BYTES, 0);
  for (const auto& deck : decks) {
    const size_t recordStart = bytes.size();
    studyPut32(bytes, deck.id);
    studyPut32(bytes, deck.parent);
    studyPut16(bytes, static_cast<uint16_t>(deck.name.size()));
    studyPut16(bytes, 0);  // flags/reserved
    bytes.insert(bytes.end(), deck.name.begin(), deck.name.end());
    bytes.resize(recordStart + Study::DECK_RECORD_BYTES, 0);
  }
  const uint32_t cardIndexOffset = static_cast<uint32_t>(bytes.size());
  const size_t cardIndexStart = bytes.size();
  bytes.resize(bytes.size() + cards.size() * Study::CARD_INDEX_BYTES, 0);
  const uint32_t dataOffset = static_cast<uint32_t>(bytes.size());
  for (size_t i = 0; i < cards.size(); ++i) {
    studySet32(bytes, cardIndexStart + i * 4, static_cast<uint32_t>(bytes.size()));
    const auto& card = cards[i];
    studyPut64(bytes, card.id);
    studyPut32(bytes, card.deckId);
    studyPut16(bytes, static_cast<uint16_t>(card.front.size()));
    studyPut16(bytes, static_cast<uint16_t>(card.back.size()));
    bytes.insert(bytes.end(), card.front.begin(), card.front.end());
    bytes.insert(bytes.end(), card.back.begin(), card.back.end());
  }
  const uint8_t magic[8] = {'C', 'P', 'S', 'T', 'U', 'D', 'Y', 0};
  std::memcpy(bytes.data(), magic, sizeof(magic));
  studySet16(bytes, 8, Study::FORMAT_VERSION);
  studySet16(bytes, 10, Study::HEADER_BYTES);
  studySet32(bytes, 12, static_cast<uint32_t>(bytes.size()));
  studySet32(bytes, 16, static_cast<uint32_t>(decks.size()));
  studySet32(bytes, 20, static_cast<uint32_t>(cards.size()));
  studySet32(bytes, 24, Study::HEADER_BYTES);
  studySet32(bytes, 28, cardIndexOffset);
  studySet32(bytes, 32, dataOffset);
  studySet32(bytes, 36, studyCrc32(bytes.data() + Study::HEADER_BYTES, bytes.size() - Study::HEADER_BYTES));
  return bytes;
}
