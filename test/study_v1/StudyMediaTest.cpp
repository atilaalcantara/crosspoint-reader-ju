#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "study/StudyMedia.h"

namespace {
struct Bytes : Study::RandomAccess {
  std::vector<uint8_t> data;
  uint64_t size() const override { return data.size(); }
  bool readAt(uint32_t offset, void* dst, size_t len) override {
    if (static_cast<uint64_t>(offset) + len > data.size()) return false;
    std::memcpy(dst, data.data() + offset, len);
    return true;
  }
};
void put(Bytes& bytes, uint64_t n, int count) {
  for (int i = 0; i < count; ++i) bytes.data.push_back(static_cast<uint8_t>(n >> (i * 8)));
}
constexpr char NAME[] = "0123456789abcdef0123456789abcdef.png";
void record(Bytes& bytes, uint64_t id, uint8_t face, const char* name = NAME) {
  put(bytes, id, 8);
  put(bytes, face, 1);
  put(bytes, 0, 1);
  put(bytes, std::strlen(name), 2);
  bytes.data.insert(bytes.data.end(), name, name + std::strlen(name));
}
Bytes index(uint32_t count = 3) {
  Bytes bytes;
  bytes.data.insert(bytes.data.end(), {'C', 'P', 'S', 'M', 'E', 'D', 'I', 'A'});
  put(bytes, 1, 2);
  put(bytes, 0, 2);
  put(bytes, 0x12345678, 4);
  put(bytes, count, 4);
  return bytes;
}
}  // namespace

TEST(StudyMedia, SelectsFacesAndRewindsWithoutKeepingIndex) {
  auto bytes = index();
  record(bytes, 1, 0);
  record(bytes, 1, 1);
  record(bytes, 3, 0);
  Study::MediaIndex media;
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  Study::MediaIndex::Names front, back;
  ASSERT_TRUE(media.imagesFor(bytes, 1, front, back));
  EXPECT_EQ(front[0], NAME);
  EXPECT_EQ(back[0], NAME);
  ASSERT_TRUE(media.imagesFor(bytes, 1, front, back));
  EXPECT_EQ(front[0], NAME);
  EXPECT_EQ(back[0], NAME);
  ASSERT_TRUE(media.imagesFor(bytes, 2, front, back));
  EXPECT_TRUE(front[0].empty());
  ASSERT_TRUE(media.imagesFor(bytes, 3, front, back));
  EXPECT_EQ(front[0], NAME);
  ASSERT_TRUE(media.imagesFor(bytes, 1, front, back));
  EXPECT_EQ(back[0], NAME);
}

TEST(StudyMedia, RejectsMismatchedCrcAndUnsafeOrMalformedRecords) {
  auto bytes = index(1);
  record(bytes, 1, 0);
  Study::MediaIndex media;
  EXPECT_FALSE(media.open(bytes, 0));
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  Study::MediaIndex::Names front, back;
  bytes.data[32] = '/';
  EXPECT_FALSE(media.imagesFor(bytes, 1, front, back));
  EXPECT_TRUE(front[0].empty());
  bytes = index(1);
  record(bytes, 1, 2);
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  EXPECT_FALSE(media.imagesFor(bytes, 1, front, back));
  bytes = index(1);
  record(bytes, 1, 0);
  bytes.data.pop_back();
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  EXPECT_FALSE(media.imagesFor(bytes, 1, front, back));
}

TEST(StudyMedia, RejectsOverflowOrderingAndTrailingBytes) {
  auto bytes = index(5);
  for (int i = 0; i < 5; ++i) record(bytes, 1, 0);
  Study::MediaIndex media;
  Study::MediaIndex::Names front, back;
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  EXPECT_FALSE(media.imagesFor(bytes, 1, front, back));
  bytes = index(2);
  record(bytes, 2, 1);
  record(bytes, 1, 0);
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  EXPECT_FALSE(media.imagesFor(bytes, 3, front, back));
  bytes = index(1);
  record(bytes, 1, 0);
  bytes.data.push_back(0);
  ASSERT_TRUE(media.open(bytes, 0x12345678));
  EXPECT_FALSE(media.imagesFor(bytes, 1, front, back));
  EXPECT_EQ(Study::MediaIndex::directoryFor("/study/foo.cps"), "/study/foo.media");
  EXPECT_TRUE(Study::MediaIndex::directoryFor("/study/../foo.cps").empty());
}
