#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Study {

inline constexpr uint16_t FORMAT_VERSION = 1;
inline constexpr uint32_t MAX_DECKS = 1024;
inline constexpr uint32_t MAX_CARDS = 100000;
inline constexpr uint16_t MAX_DECK_NAME_BYTES = 96;
inline constexpr uint16_t MAX_FACE_BYTES = 4096;
inline constexpr uint32_t HEADER_BYTES = 40;
inline constexpr uint32_t DECK_RECORD_BYTES = 108;
inline constexpr uint32_t CARD_INDEX_BYTES = 4;
inline constexpr uint32_t CARD_INDEX_RAM_BYTES = 2 * sizeof(uint32_t);  // payload offset + deck ID

enum class Error : uint8_t {
  None,
  Io,
  BadMagic,
  UnsupportedVersion,
  InvalidHeader,
  InvalidChecksum,
  InvalidDeck,
  InvalidCard,
  InvalidUtf8,
  LimitExceeded,
  OutOfMemory,
  NotFound,
};

// readAt must either fill the complete requested range or return false.
class RandomAccess {
 public:
  virtual ~RandomAccess() = default;
  virtual uint64_t size() const = 0;
  virtual bool readAt(uint32_t offset, void* destination, size_t length) = 0;
};

class WritableRandomAccess : public RandomAccess {
 public:
  virtual bool writeAt(uint32_t offset, const void* source, size_t length) = 0;
  virtual bool flush() = 0;
};

struct DeckInfo {
  uint32_t id = 0;
  uint32_t parentId = 0;
  std::string name;
  uint32_t cardCount = 0;
  uint32_t subtreeCardCount = 0;
};

struct Card {
  uint64_t id = 0;
  uint32_t deckId = 0;
  std::string front;
  std::string back;
};

// A parsed catalog owns only deck metadata and the compact card-offset index.
// It never stores card text. open() validates every record and the package CRC.
class Package {
 public:
  bool open(RandomAccess& source, Error& error);
  uint32_t deckCount() const { return static_cast<uint32_t>(decks_.size()); }
  uint32_t cardCount() const { return static_cast<uint32_t>(cardOffsets_.size()); }
  uint32_t packageCrc() const { return packageCrc_; }
  uint32_t indexRamBytes() const { return static_cast<uint32_t>(cardOffsets_.size() * CARD_INDEX_RAM_BYTES); }
  const DeckInfo* deck(uint32_t id) const;
  const std::vector<DeckInfo>& decks() const { return decks_; }
  void releaseCardIndex() {
    cardOffsets_.clear();
    cardOffsets_.shrink_to_fit();
    cardDeckIds_.clear();
    cardDeckIds_.shrink_to_fit();
  }
  bool cardIdAt(RandomAccess& source, uint32_t index, uint64_t& cardId, Error& error) const;
  bool loadCard(RandomAccess& source, uint32_t index, Card& out, Error& error) const;
  bool cardBelongsToDeck(uint32_t cardIndex, uint32_t deckId, bool includeDescendants) const;

 private:
  std::vector<DeckInfo> decks_;
  std::vector<uint32_t> cardOffsets_;
  std::vector<uint32_t> cardDeckIds_;
  uint32_t packageCrc_ = 0;
  uint32_t declaredFileSize_ = 0;
};

struct CatalogFile {
  std::string path;
  Error error = Error::None;
  Package package;
  bool valid() const { return error == Error::None; }
};

// A bad package is retained as a diagnostic entry but does not invalidate
// other files discovered under /study. Package paths are the namespace for IDs.
class Catalog {
 public:
  void clear() { files_.clear(); }
  void releaseCardIndexes();
  bool addFile(const std::string& path, RandomAccess& source);
  void addError(const std::string& path, Error error);
  const std::vector<CatalogFile>& files() const { return files_; }
  uint32_t validPackageCount() const;

 private:
  std::vector<CatalogFile> files_;
};

bool isValidUtf8(const uint8_t* bytes, size_t length);
const char* errorName(Error error);

}  // namespace Study
