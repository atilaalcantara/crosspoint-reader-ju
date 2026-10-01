#include <gtest/gtest.h>
#include <util/HighlightStore.h>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Highlights = std::vector<HighlightEntry>;
const std::filesystem::path ROOT = std::filesystem::temp_directory_path() / "crosspoint-highlight-store-test";

HighlightEntry entry(uint32_t id, uint16_t spine, uint32_t start, uint32_t end, std::string quote,
                     std::string note = {}) {
  return {id, spine, start, end, std::move(quote), std::move(note)};
}

template <typename T>
void appendLittleEndian(std::vector<uint8_t>& bytes, const T value) {
  static_assert(std::is_unsigned_v<T>);
  for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

uint32_t crc32(const std::vector<uint8_t>& bytes) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const uint8_t byte : bytes) {
    crc ^= byte;
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool writeLegacyV1(const std::filesystem::path& file, const std::string& book,
                   const std::vector<HighlightEntry>& entries) {
  std::vector<uint8_t> bytes{'H', 'L', 'T', '1', 1};
  appendLittleEndian(bytes, static_cast<uint16_t>(entries.size()));
  appendLittleEndian(bytes, static_cast<uint16_t>(book.size()));
  bytes.insert(bytes.end(), book.begin(), book.end());
  for (const auto& item : entries) {
    appendLittleEndian(bytes, item.id);
    appendLittleEndian(bytes, item.spineIndex);
    appendLittleEndian(bytes, item.startOffset);
    appendLittleEndian(bytes, item.endOffset);
    appendLittleEndian(bytes, static_cast<uint16_t>(item.quote.size()));
    bytes.insert(bytes.end(), item.quote.begin(), item.quote.end());
  }
  appendLittleEndian(bytes, crc32(bytes));
  std::filesystem::create_directories(file.parent_path());
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

void corruptByte(const std::filesystem::path& path, const std::streamoff offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekg(offset);
  char byte = 0;
  file.read(&byte, 1);
  byte ^= static_cast<char>(0xFF);
  file.seekp(offset);
  file.write(&byte, 1);
}

void setByte(const std::filesystem::path& path, const std::streamoff offset, const char byte) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(offset);
  file.write(&byte, 1);
}

}  // namespace

TEST(HighlightStoreTest, MissingFileIsAnEmptyCollection) {
  std::filesystem::remove_all(ROOT);
  Highlights loaded{entry(9, 4, 1, 2, "old")};
  ASSERT_TRUE(HighlightStore::load("/books/missing.epub", loaded));
  EXPECT_TRUE(loaded.empty());
}

TEST(HighlightStoreTest, ListsOnlyValidNonemptyBooksWithoutTrustingSidecarNames) {
  std::filesystem::remove_all(ROOT);
  const std::string first = "/books/A.epub";
  const std::string second = "/books/B.epub";
  ASSERT_TRUE(HighlightStore::save(second, {entry(1, 0, 1, 2, "B")}));
  ASSERT_TRUE(HighlightStore::save(first, {entry(2, 1, 4, 5, "A")}));
  ASSERT_TRUE(HighlightStore::save("/books/empty.epub", {}));
  std::vector<std::string> paths;
  ASSERT_TRUE(HighlightStore::listBookPaths(paths));
  EXPECT_EQ(paths, (std::vector<std::string>{first, second}));

  const auto file = ROOT / std::filesystem::path(HighlightStore::getFilePath(second)).relative_path();
  corruptByte(file, 0);
  EXPECT_FALSE(HighlightStore::listBookPaths(paths));
  EXPECT_EQ(paths, (std::vector<std::string>{first}));
}

TEST(HighlightStoreTest, DiscoversAndRecoversAnOrphanedBackup) {
  std::filesystem::remove_all(ROOT);
  const std::string book = "/books/recover.epub";
  ASSERT_TRUE(HighlightStore::save(book, {entry(3, 1, 10, 12, "quote")}));
  const auto canonical = ROOT / std::filesystem::path(HighlightStore::getFilePath(book)).relative_path();
  std::filesystem::rename(canonical, canonical.string() + ".bak");
  std::vector<std::string> paths;
  EXPECT_TRUE(HighlightStore::listBookPaths(paths));
  EXPECT_EQ(paths, (std::vector<std::string>{book}));
  EXPECT_TRUE(std::filesystem::exists(canonical));
}

TEST(HighlightStoreTest, RoundTripsSpinesAndResolvesOnlyOverlappingWordRanges) {
  std::filesystem::remove_all(ROOT);
  const std::string book = "/books/reader.epub";
  const Highlights expected{entry(1, 0, 12, 17, "first", "définition 🐟"),
                            entry(2, 1, 40, 46, "no note"), entry(3, 3, 80, 88, "second", "review point")};
  ASSERT_TRUE(HighlightStore::save(book, expected));

  Highlights loaded;
  ASSERT_TRUE(HighlightStore::load(book, loaded));
  ASSERT_EQ(loaded.size(), 3u);
  EXPECT_EQ(loaded[0].id, 1u);
  EXPECT_EQ(loaded[0].spineIndex, 0u);
  EXPECT_EQ(loaded[0].startOffset, 12u);
  EXPECT_EQ(loaded[0].endOffset, 17u);
  EXPECT_EQ(loaded[0].quote, "first");
  EXPECT_EQ(loaded[0].note, "définition 🐟");
  EXPECT_EQ(loaded[1].spineIndex, 1u);
  EXPECT_TRUE(loaded[1].note.empty());
  EXPECT_EQ(loaded[2].spineIndex, 3u);
  EXPECT_EQ(loaded[2].quote, "second");
  EXPECT_EQ(loaded[2].note, "review point");

  EXPECT_TRUE(loaded[0].overlaps(0, 14, 16));
  EXPECT_FALSE(loaded[0].overlaps(0, 17, 19));  // end is exclusive
  EXPECT_FALSE(loaded[0].overlaps(3, 14, 16));  // spine identity is part of the anchor
  EXPECT_TRUE(loaded[2].overlaps(3, 86, 90));
}

TEST(HighlightStoreTest, LoadsLegacyV1WithoutNotesAndMigratesOnNextSave) {
  std::filesystem::remove_all(ROOT);
  const std::string book = "/books/legacy.epub";
  const auto file = ROOT / std::filesystem::path(HighlightStore::getFilePath(book)).relative_path();
  ASSERT_TRUE(writeLegacyV1(file, book, {entry(4, 1, 23, 31, "legacy quote")}));

  Highlights loaded;
  ASSERT_TRUE(HighlightStore::load(book, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].quote, "legacy quote");
  EXPECT_TRUE(loaded[0].note.empty());

  loaded[0].note = "migrated note";
  ASSERT_TRUE(HighlightStore::save(book, loaded));
  {
    std::ifstream input(file, std::ios::binary);
    ASSERT_TRUE(input.good());
    input.seekg(4);
    EXPECT_EQ(input.get(), HighlightStore::FORMAT_VERSION);
  }

  Highlights reloaded;
  ASSERT_TRUE(HighlightStore::load(book, reloaded));
  ASSERT_EQ(reloaded.size(), 1u);
  EXPECT_EQ(reloaded[0].note, "migrated note");
}

TEST(HighlightStoreTest, RejectsBadChecksumAndLeavesCallerStateUntouched) {
  std::filesystem::remove_all(ROOT);
  const std::string book = "/books/corrupt.epub";
  ASSERT_TRUE(HighlightStore::save(book, {entry(1, 0, 2, 5, "quote")}));
  const auto file = ROOT / std::filesystem::path(HighlightStore::getFilePath(book)).relative_path();
  const auto fileSize = std::filesystem::file_size(file);
  corruptByte(file, static_cast<std::streamoff>(fileSize - 1));

  Highlights loaded{entry(99, 9, 10, 12, "keep")};
  EXPECT_FALSE(HighlightStore::load(book, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].id, 99u);
}

TEST(HighlightStoreTest, RejectsUnknownVersionAndKeepsOtherBookStateSeparate) {
  std::filesystem::remove_all(ROOT);
  const std::string firstBook = "/books/version.epub";
  const std::string secondBook = "/books/other.epub";
  ASSERT_TRUE(HighlightStore::save(firstBook, {entry(1, 0, 2, 5, "first book")}));

  Highlights otherBook{entry(8, 9, 20, 24, "sentinel")};
  ASSERT_TRUE(HighlightStore::load(secondBook, otherBook));
  EXPECT_TRUE(otherBook.empty());

  const auto file = ROOT / std::filesystem::path(HighlightStore::getFilePath(firstBook)).relative_path();
  setByte(file, 4, static_cast<char>(HighlightStore::FORMAT_VERSION + 1));
  Highlights loaded{entry(99, 9, 10, 12, "keep")};
  EXPECT_FALSE(HighlightStore::load(firstBook, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].id, 99u);
}

TEST(HighlightStoreTest, RestoresOnlyAValidatedBackupWhenCanonicalIsCorrupt) {
  std::filesystem::remove_all(ROOT);
  const std::string book = "/books/recover.epub";
  const Highlights expected{entry(7, 2, 31, 40, "recover me")};
  ASSERT_TRUE(HighlightStore::save(book, expected));
  const auto canonical = ROOT / std::filesystem::path(HighlightStore::getFilePath(book)).relative_path();
  const auto backup = canonical.string() + ".bak";
  std::filesystem::copy_file(canonical, backup);
  corruptByte(canonical, 0);

  Highlights loaded;
  ASSERT_TRUE(HighlightStore::load(book, loaded));
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].quote, "recover me");
  EXPECT_TRUE(std::filesystem::exists(canonical));
  EXPECT_FALSE(std::filesystem::exists(backup));
}

TEST(HighlightStoreTest, RejectsDuplicateIdsAndOutOfRangeRecordsBeforeWriting) {
  std::filesystem::remove_all(ROOT);
  const std::string book = "/books/invalid.epub";
  EXPECT_FALSE(HighlightStore::save(book, {entry(1, 0, 1, 3, "a"), entry(1, 1, 4, 6, "b")}));
  EXPECT_FALSE(HighlightStore::save(book, {entry(1, 0, 5, 5, "empty range")}));
  EXPECT_FALSE(HighlightStore::save(book, {entry(1, 0, 1, 3, std::string(513, 'x'))}));
  EXPECT_FALSE(HighlightStore::save(book, {entry(1, 0, 1, 3, "quote", std::string(513, 'x'))}));
  EXPECT_FALSE(HighlightStore::save(book, {entry(1, 0, 1, 3, "quote", std::string("\xC3", 1))}));
  EXPECT_FALSE(std::filesystem::exists(ROOT / ".crosspoint" / "highlights"));
}
