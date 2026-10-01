#include "StudyPackage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>

namespace Study {
namespace {

constexpr uint8_t MAGIC[8] = {'C', 'P', 'S', 'T', 'U', 'D', 'Y', 0};
constexpr uint32_t MAX_FILE_BYTES = 0xFFFFFFFFu;
constexpr uint32_t CRC_POLYNOMIAL = 0xEDB88320u;

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t u64(const uint8_t* p) { return static_cast<uint64_t>(u32(p)) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

uint32_t updateCrc(uint32_t crc, const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (CRC_POLYNOMIAL & (0u - (crc & 1u)));
  }
  return crc;
}

bool rangeFits(uint32_t offset, uint64_t length, uint32_t total) {
  return offset <= total && length <= static_cast<uint64_t>(total - offset);
}

bool readCrc(RandomAccess& source, uint32_t offset, uint32_t length, uint32_t& result) {
  std::array<uint8_t, 256> chunk{};
  uint32_t crc = 0xFFFFFFFFu;
  while (length > 0) {
    const size_t count = std::min<size_t>(chunk.size(), length);
    if (!source.readAt(offset, chunk.data(), count)) return false;
    crc = updateCrc(crc, chunk.data(), count);
    offset += static_cast<uint32_t>(count);
    length -= static_cast<uint32_t>(count);
  }
  result = ~crc;
  return true;
}

bool readExact(RandomAccess& source, uint32_t offset, void* dst, size_t length) {
  return source.readAt(offset, dst, length);
}

bool isValidStudyText(const uint8_t* bytes, size_t length) {
  if (!isValidUtf8(bytes, length)) return false;
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] == 0 || bytes[i] == '\r' || (bytes[i] < 0x20 && bytes[i] != '\n' && bytes[i] != '\t')) return false;
  }
  return true;
}

bool descendsFrom(const std::vector<DeckInfo>& decks, uint32_t candidate, uint32_t ancestor) {
  for (uint32_t hops = 0; hops <= decks.size(); ++hops) {
    if (candidate == ancestor) return true;
    const auto it = std::find_if(decks.begin(), decks.end(), [candidate](const DeckInfo& d) { return d.id == candidate; });
    if (it == decks.end() || it->parentId == 0) return false;
    candidate = it->parentId;
  }
  return false;
}

}  // namespace

bool isValidUtf8(const uint8_t* bytes, size_t length) {
  size_t i = 0;
  while (i < length) {
    const uint8_t a = bytes[i++];
    if (a <= 0x7F) continue;
    uint32_t codepoint = 0;
    size_t continuation = 0;
    if (a >= 0xC2 && a <= 0xDF) {
      codepoint = a & 0x1F;
      continuation = 1;
    } else if (a >= 0xE0 && a <= 0xEF) {
      codepoint = a & 0x0F;
      continuation = 2;
    } else if (a >= 0xF0 && a <= 0xF4) {
      codepoint = a & 0x07;
      continuation = 3;
    } else {
      return false;
    }
    if (continuation > length - i) return false;
    const uint8_t first = bytes[i];
    if ((a == 0xE0 && first < 0xA0) || (a == 0xED && first >= 0xA0) || (a == 0xF0 && first < 0x90) ||
        (a == 0xF4 && first >= 0x90)) {
      return false;
    }
    for (size_t j = 0; j < continuation; ++j) {
      const uint8_t b = bytes[i++];
      if ((b & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (b & 0x3F);
    }
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
  }
  return true;
}

const char* errorName(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::Io: return "io";
    case Error::BadMagic: return "bad-magic";
    case Error::UnsupportedVersion: return "unsupported-version";
    case Error::InvalidHeader: return "invalid-header";
    case Error::InvalidChecksum: return "invalid-checksum";
    case Error::InvalidDeck: return "invalid-deck";
    case Error::InvalidCard: return "invalid-card";
    case Error::InvalidUtf8: return "invalid-utf8";
    case Error::LimitExceeded: return "limit-exceeded";
    case Error::OutOfMemory: return "out-of-memory";
    case Error::NotFound: return "not-found";
  }
  return "unknown";
}

bool Package::open(RandomAccess& source, Error& error) {
  error = Error::None;
  decks_.clear();
  cardOffsets_.clear();
  cardDeckIds_.clear();
  packageCrc_ = 0;
  std::array<uint8_t, HEADER_BYTES> header{};
  if (source.size() < HEADER_BYTES || source.size() > MAX_FILE_BYTES || !readExact(source, 0, header.data(), header.size())) {
    error = Error::Io;
    return false;
  }
  if (std::memcmp(header.data(), MAGIC, sizeof(MAGIC)) != 0) {
    error = Error::BadMagic;
    return false;
  }
  if (u16(header.data() + 8) != FORMAT_VERSION) {
    error = Error::UnsupportedVersion;
    return false;
  }
  const uint16_t headerSize = u16(header.data() + 10);
  const uint32_t fileSize = u32(header.data() + 12);
  const uint32_t deckCount = u32(header.data() + 16);
  const uint32_t cardCount = u32(header.data() + 20);
  const uint32_t deckOffset = u32(header.data() + 24);
  const uint32_t cardIndexOffset = u32(header.data() + 28);
  const uint32_t dataOffset = u32(header.data() + 32);
  const uint32_t expectedCrc = u32(header.data() + 36);
  const uint64_t deckBytes = static_cast<uint64_t>(deckCount) * DECK_RECORD_BYTES;
  const uint64_t indexBytes = static_cast<uint64_t>(cardCount) * CARD_INDEX_BYTES;
  if (headerSize != HEADER_BYTES || fileSize != source.size() || deckCount == 0 || deckCount > MAX_DECKS ||
      cardCount > MAX_CARDS || deckOffset != HEADER_BYTES || !rangeFits(deckOffset, deckBytes, fileSize) ||
      cardIndexOffset != deckOffset + deckBytes || !rangeFits(cardIndexOffset, indexBytes, fileSize) ||
      dataOffset != cardIndexOffset + indexBytes || dataOffset > fileSize) {
    error = Error::InvalidHeader;
    return false;
  }
  uint32_t actualCrc = 0;
  if (!readCrc(source, HEADER_BYTES, fileSize - HEADER_BYTES, actualCrc)) {
    error = Error::Io;
    return false;
  }
  if (actualCrc != expectedCrc) {
    error = Error::InvalidChecksum;
    return false;
  }
  packageCrc_ = actualCrc;
  declaredFileSize_ = fileSize;
  decks_.reserve(deckCount);
  for (uint32_t i = 0; i < deckCount; ++i) {
    std::array<uint8_t, DECK_RECORD_BYTES> record{};
    if (!readExact(source, deckOffset + i * DECK_RECORD_BYTES, record.data(), record.size())) {
      error = Error::Io;
      decks_.clear();
      return false;
    }
    const uint32_t id = u32(record.data());
    const uint32_t parentId = u32(record.data() + 4);
    const uint16_t nameLength = u16(record.data() + 8);
    bool paddingIsZero = u16(record.data() + 10) == 0 && nameLength <= MAX_DECK_NAME_BYTES;
    if (nameLength <= MAX_DECK_NAME_BYTES) {
      for (size_t p = 12 + nameLength; p < record.size(); ++p) paddingIsZero = paddingIsZero && record[p] == 0;
    }
    if (id == 0 || nameLength == 0 || nameLength > MAX_DECK_NAME_BYTES ||
        !paddingIsZero || !isValidStudyText(record.data() + 12, nameLength)) {
      error = nameLength > MAX_DECK_NAME_BYTES ? Error::LimitExceeded : Error::InvalidDeck;
      decks_.clear();
      return false;
    }
    for (const auto& previous : decks_) {
      if (previous.id == id) {
        error = Error::InvalidDeck;
        decks_.clear();
        return false;
      }
    }
    DeckInfo deckInfo;
    deckInfo.id = id;
    deckInfo.parentId = parentId;
    deckInfo.name.assign(reinterpret_cast<const char*>(record.data() + 12), nameLength);
    decks_.push_back(std::move(deckInfo));
  }
  for (const auto& item : decks_) {
    if ((item.parentId != 0 && !deck(item.parentId)) || item.parentId == item.id) {
      error = Error::InvalidDeck;
      decks_.clear();
      return false;
    }
  }
  for (const auto& item : decks_) {
    uint32_t cursor = item.parentId;
    for (uint32_t hops = 0; cursor != 0 && hops <= deckCount; ++hops) {
      const DeckInfo* parent = deck(cursor);
      if (!parent || parent->id == item.id || hops == deckCount) {
        error = Error::InvalidDeck;
        decks_.clear();
        return false;
      }
      cursor = parent->parentId;
    }
  }
  cardOffsets_.reserve(cardCount);
  cardDeckIds_.reserve(cardCount);
  std::unique_ptr<uint8_t[]> text(new (std::nothrow) uint8_t[MAX_FACE_BYTES * 2]);
  if (!text) {
    error = Error::OutOfMemory;
    return false;
  }
  uint64_t previousCardId = 0;
  for (uint32_t i = 0; i < cardCount; ++i) {
    std::array<uint8_t, CARD_INDEX_BYTES> offsetBytes{};
    const uint32_t indexPos = cardIndexOffset + i * CARD_INDEX_BYTES;
    if (!readExact(source, indexPos, offsetBytes.data(), offsetBytes.size())) {
      error = Error::Io;
      cardOffsets_.clear();
      return false;
    }
    const uint32_t offset = u32(offsetBytes.data());
    std::array<uint8_t, 16> prefix{};
    if (offset < dataOffset || !rangeFits(offset, prefix.size(), fileSize) || !readExact(source, offset, prefix.data(), prefix.size())) {
      error = Error::InvalidCard;
      cardOffsets_.clear();
      return false;
    }
    const uint64_t id = u64(prefix.data());
    const uint32_t deckId = u32(prefix.data() + 8);
    const uint16_t frontLength = u16(prefix.data() + 12);
    const uint16_t backLength = u16(prefix.data() + 14);
    const uint64_t recordLength = 16ull + frontLength + backLength;
    if (id == 0 || id <= previousCardId || !deck(deckId) || frontLength > MAX_FACE_BYTES || backLength > MAX_FACE_BYTES ||
        !rangeFits(offset, recordLength, fileSize)) {
      error = (frontLength > MAX_FACE_BYTES || backLength > MAX_FACE_BYTES) ? Error::LimitExceeded : Error::InvalidCard;
      cardOffsets_.clear();
      return false;
    }
    // Records are required to be contiguous and in index order, allowing a sequential validation
    // without retaining another per-card structure.
    if (i == 0 && offset != dataOffset) {
      error = Error::InvalidCard;
      cardOffsets_.clear();
      return false;
    }
    if (i > 0) {
      const uint32_t previous = cardOffsets_.back();
      std::array<uint8_t, 16> previousPrefix{};
      if (!readExact(source, previous, previousPrefix.data(), previousPrefix.size()) ||
          offset != previous + 16u + u16(previousPrefix.data() + 12) + u16(previousPrefix.data() + 14)) {
        error = Error::InvalidCard;
        cardOffsets_.clear();
        return false;
      }
    }
    const size_t textBytes = static_cast<size_t>(frontLength) + backLength;
    if (!readExact(source, offset + 16, text.get(), textBytes)) {
      error = Error::Io;
      cardOffsets_.clear();
      return false;
    }
    if (!isValidStudyText(text.get(), frontLength) || !isValidStudyText(text.get() + frontLength, backLength)) {
      error = Error::InvalidUtf8;
      cardOffsets_.clear();
      return false;
    }
    cardOffsets_.push_back(offset);
    cardDeckIds_.push_back(deckId);
    previousCardId = id;
    auto found = std::find_if(decks_.begin(), decks_.end(), [deckId](const DeckInfo& d) { return d.id == deckId; });
    if (found == decks_.end()) {
      error = Error::InvalidDeck;
      return false;
    }
    ++found->cardCount;
  }
  bool recordsEndAtEof = cardCount == 0 && dataOffset == fileSize;
  if (cardCount > 0) {
    std::array<uint8_t, 16> lastPrefix{};
    if (!readExact(source, cardOffsets_.back(), lastPrefix.data(), lastPrefix.size())) {
      error = Error::Io;
      return false;
    }
    const uint32_t lastLength = static_cast<uint32_t>(u16(lastPrefix.data() + 12)) +
                                static_cast<uint32_t>(u16(lastPrefix.data() + 14));
    recordsEndAtEof = static_cast<uint64_t>(cardOffsets_.back()) + 16 + lastLength == fileSize;
  }
  if (!recordsEndAtEof) {
    error = Error::InvalidCard;
    cardOffsets_.clear();
    return false;
  }
  for (auto& item : decks_) {
    for (const auto& possibleChild : decks_) {
      if (possibleChild.id != item.id && descendsFrom(decks_, possibleChild.id, item.id)) {
        item.subtreeCardCount += possibleChild.cardCount;
      }
    }
    item.subtreeCardCount += item.cardCount;
  }
  return true;
}

const DeckInfo* Package::deck(uint32_t id) const {
  const auto it = std::find_if(decks_.begin(), decks_.end(), [id](const DeckInfo& item) { return item.id == id; });
  return it == decks_.end() ? nullptr : &*it;
}

bool Package::cardIdAt(RandomAccess& source, const uint32_t index, uint64_t& cardId, Error& error) const {
  error = Error::None;
  if (index >= cardOffsets_.size() || source.size() != declaredFileSize_) {
    error = Error::NotFound;
    return false;
  }
  std::array<uint8_t, 8> bytes{};
  if (!readExact(source, cardOffsets_[index], bytes.data(), bytes.size())) {
    error = Error::Io;
    return false;
  }
  cardId = u64(bytes.data());
  return cardId != 0;
}

bool Package::loadCard(RandomAccess& source, uint32_t index, Card& out, Error& error) const {
  error = Error::None;
  if (index >= cardOffsets_.size() || source.size() != declaredFileSize_) {
    error = Error::NotFound;
    return false;
  }
  std::array<uint8_t, 16> prefix{};
  const uint32_t offset = cardOffsets_[index];
  if (!readExact(source, offset, prefix.data(), prefix.size())) {
    error = Error::Io;
    return false;
  }
  const uint16_t frontLength = u16(prefix.data() + 12);
  const uint16_t backLength = u16(prefix.data() + 14);
  if (frontLength > MAX_FACE_BYTES || backLength > MAX_FACE_BYTES) {
    error = Error::LimitExceeded;
    return false;
  }
  std::unique_ptr<uint8_t[]> text(new (std::nothrow) uint8_t[MAX_FACE_BYTES * 2]);
  if (!text) {
    error = Error::OutOfMemory;
    return false;
  }
  if (!readExact(source, offset + 16, text.get(), static_cast<size_t>(frontLength) + backLength)) {
    error = Error::Io;
    return false;
  }
  if (!isValidStudyText(text.get(), frontLength) || !isValidStudyText(text.get() + frontLength, backLength)) {
    error = Error::InvalidUtf8;
    return false;
  }
  Card loaded;
  loaded.id = u64(prefix.data());
  loaded.deckId = cardDeckIds_[index];
  loaded.front.assign(reinterpret_cast<const char*>(text.get()), frontLength);
  loaded.back.assign(reinterpret_cast<const char*>(text.get() + frontLength), backLength);
  out = std::move(loaded);
  return true;
}

bool Package::cardBelongsToDeck(uint32_t cardIndex, uint32_t deckId, bool includeDescendants) const {
  if (cardIndex >= cardDeckIds_.size() || !this->deck(deckId)) return false;
  return includeDescendants ? descendsFrom(decks_, cardDeckIds_[cardIndex], deckId) : cardDeckIds_[cardIndex] == deckId;
}

bool Catalog::addFile(const std::string& path, RandomAccess& source) {
  CatalogFile entry;
  entry.path = path;
  entry.package.open(source, entry.error);
  files_.push_back(std::move(entry));
  return files_.back().valid();
}

void Catalog::addError(const std::string& path, Error error) {
  CatalogFile entry;
  entry.path = path;
  entry.error = error;
  files_.push_back(std::move(entry));
}

void Catalog::releaseCardIndexes() {
  for (auto& file : files_) {
    if (file.valid()) file.package.releaseCardIndex();
  }
}

uint32_t Catalog::validPackageCount() const {
  uint32_t count = 0;
  for (const auto& file : files_) {
    if (file.valid()) ++count;
  }
  return count;
}

}  // namespace Study
