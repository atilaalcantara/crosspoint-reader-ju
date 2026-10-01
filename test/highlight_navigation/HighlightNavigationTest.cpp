#include <util/HighlightNavigation.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {

HighlightEntry entry(const uint32_t id, const uint16_t spine, const uint32_t start, const uint32_t end,
                     std::string quote) {
  return {id, spine, start, end, std::move(quote)};
}

}  // namespace

TEST(HighlightNavigationTest, SortsBySpineThenTextOffsetAndUsesStableTieBreakers) {
  std::vector<HighlightEntry> highlights{entry(7, 1, 0, 5, "spine one"), entry(9, 0, 30, 36, "later"),
                                         entry(3, 0, 10, 18, "first"), entry(5, 0, 10, 16, "same start"),
                                         entry(2, 0, 10, 16, "same location")};

  std::sort(highlights.begin(), highlights.end(), HighlightNavigation::comesBefore);

  ASSERT_EQ(highlights.size(), 5u);
  EXPECT_EQ(highlights[0].id, 2u);
  EXPECT_EQ(highlights[1].id, 5u);
  EXPECT_EQ(highlights[2].id, 3u);
  EXPECT_EQ(highlights[3].id, 9u);
  EXPECT_EQ(highlights[4].id, 7u);
}

TEST(HighlightNavigationTest, QuotePreviewTruncatesOnUnicodeCharacterBoundary) {
  EXPECT_EQ(HighlightNavigation::quotePreview("", 3), "");
  EXPECT_EQ(HighlightNavigation::quotePreview("é漢abc", 2), "é漢…");
  EXPECT_EQ(HighlightNavigation::quotePreview("short", 8), "short");
}
