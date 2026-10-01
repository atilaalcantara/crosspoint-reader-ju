#include <Epub/Epub/VisibleTextOffsetResolver.h>
#include <gtest/gtest.h>

#include <vector>

TEST(VisibleTextOffsetResolverTest, EmptyIndexHasNoResult) {
  const VisibleTextOffsetResolver resolver(0);
  EXPECT_FALSE(resolver.result().has_value());
}

TEST(VisibleTextOffsetResolverTest, FindsContainingPageAndUsesLastPageAtBoundaryByDefault) {
  const std::vector<uint32_t> pageStarts{0, 10, 25, 50};
  VisibleTextOffsetResolver beforeBoundary(24);
  for (uint16_t i = 0; i < pageStarts.size(); ++i) {
    if (beforeBoundary.consume(i, pageStarts[i])) break;
  }
  EXPECT_EQ(beforeBoundary.result(), 1u);

  VisibleTextOffsetResolver atBoundary(25);
  for (uint16_t i = 0; i < pageStarts.size(); ++i) {
    if (atBoundary.consume(i, pageStarts[i])) break;
  }
  EXPECT_EQ(atBoundary.result(), 2u);
}

TEST(VisibleTextOffsetResolverTest, OffsetBeforeFirstPageStartFallsBackToFirstPage) {
  const std::vector<uint32_t> pageStarts{10, 25, 50};
  VisibleTextOffsetResolver resolver(0);
  for (uint16_t i = 0; i < pageStarts.size(); ++i) {
    if (resolver.consume(i, pageStarts[i])) break;
  }
  EXPECT_EQ(resolver.result(), 0u);
}

TEST(VisibleTextOffsetResolverTest, ChoosesFirstOfDuplicateStartsWhenRequested) {
  const std::vector<uint32_t> pageStarts{0, 10, 10, 30};
  VisibleTextOffsetResolver first(10, true);
  for (uint16_t i = 0; i < pageStarts.size(); ++i) {
    if (first.consume(i, pageStarts[i])) break;
  }
  EXPECT_EQ(first.result(), 1u);

  VisibleTextOffsetResolver last(10, false);
  for (uint16_t i = 0; i < pageStarts.size(); ++i) {
    if (last.consume(i, pageStarts[i])) break;
  }
  EXPECT_EQ(last.result(), 2u);
}

TEST(VisibleTextOffsetResolverTest, OffsetAfterLastPageMapsToLastPageForCompleteSections) {
  const std::vector<uint32_t> pageStarts{0, 10, 25};
  VisibleTextOffsetResolver resolver(100);
  for (uint16_t i = 0; i < pageStarts.size(); ++i) {
    if (resolver.consume(i, pageStarts[i])) break;
  }
  EXPECT_EQ(resolver.result(), 2u);
}
