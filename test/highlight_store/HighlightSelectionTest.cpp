#include <HighlightSelection.h>
#include <gtest/gtest.h>

#include <vector>

using HighlightSelection::WordBox;

TEST(HighlightSelectionTest, HitTestUsesScreenBoxesAndFingerSlop) {
  const std::vector<WordBox> words{{10, 20, 24, 18, 4, 8}, {42, 20, 20, 18, 9, 13}};
  EXPECT_EQ(HighlightSelection::hitTest(words, 11, 21), 0u);
  EXPECT_EQ(HighlightSelection::hitTest(words, 39, 25), 1u);
  EXPECT_FALSE(HighlightSelection::hitTest(words, 80, 25).has_value());
}

TEST(HighlightSelectionTest, ReverseDragNormalizesByTextOffsetsNotScreenOrder) {
  // Screen order may be RTL while offsets remain in source order.
  const std::vector<WordBox> words{{50, 20, 22, 18, 20, 24}, {20, 20, 25, 18, 10, 16}};
  const auto forward = HighlightSelection::normalize(words, 1, 0);
  const auto reverse = HighlightSelection::normalize(words, 0, 1);
  ASSERT_TRUE(forward.has_value());
  ASSERT_TRUE(reverse.has_value());
  EXPECT_EQ(forward->start, 10u);
  EXPECT_EQ(forward->end, 24u);
  EXPECT_EQ(reverse->start, forward->start);
  EXPECT_EQ(reverse->end, forward->end);
  EXPECT_TRUE(HighlightSelection::overlaps(*forward, 16, 21));
  EXPECT_FALSE(HighlightSelection::overlaps(*forward, 24, 29));
}

TEST(HighlightSelectionTest, LongPressDragMovesEndpointUntilReleaseAndKeepsSelectionAvailable) {
  HighlightSelection::DragGesture gesture;
  gesture.pointerDown(1);  // DOWN(A)
  EXPECT_FALSE(gesture.active());
  ASSERT_TRUE(gesture.activateLongPress());  // long-press threshold
  ASSERT_TRUE(gesture.moveTo(2));             // MOVE(B)
  ASSERT_TRUE(gesture.moveTo(4));             // MOVE(C)
  EXPECT_EQ(gesture.anchor(), 1u);
  EXPECT_EQ(gesture.focus(), 4u);
  EXPECT_TRUE(gesture.release(3));  // UP with a final hit-test sample
  EXPECT_FALSE(gesture.tracking());
  EXPECT_TRUE(gesture.active());
  EXPECT_EQ(gesture.focus(), 3u);

  const std::vector<WordBox> words{{10, 20, 14, 18, 0, 4}, {30, 20, 14, 18, 5, 9},
                                   {50, 20, 14, 18, 10, 14}, {70, 20, 14, 18, 15, 19}};
  const auto range = HighlightSelection::normalize(words, gesture.anchor(), gesture.focus());
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->start, 5u);
  EXPECT_EQ(range->end, 19u);
}

TEST(HighlightSelectionTest, ReverseLongPressDragKeepsAnchorAndNormalizesRange) {
  HighlightSelection::DragGesture gesture;
  gesture.pointerDown(3);
  ASSERT_TRUE(gesture.activateLongPress());
  ASSERT_TRUE(gesture.moveTo(2));
  ASSERT_TRUE(gesture.moveTo(0));
  EXPECT_TRUE(gesture.release(0));

  const std::vector<WordBox> words{{10, 20, 14, 18, 0, 4}, {30, 20, 14, 18, 5, 9},
                                   {50, 20, 14, 18, 10, 14}, {70, 20, 14, 18, 15, 19}};
  const auto range = HighlightSelection::normalize(words, gesture.anchor(), gesture.focus());
  ASSERT_TRUE(range.has_value());
  EXPECT_EQ(range->start, 0u);
  EXPECT_EQ(range->end, 19u);
}

TEST(HighlightSelectionTest, ReleasingOutsideTextKeepsTheLastValidFocus) {
  HighlightSelection::DragGesture gesture;
  gesture.pointerDown(0);
  ASSERT_TRUE(gesture.activateLongPress());
  ASSERT_TRUE(gesture.moveTo(2));
  EXPECT_TRUE(gesture.release());
  EXPECT_EQ(gesture.focus(), 2u);
}

TEST(HighlightSelectionTest, QuoteSeparatesCompactParagraphsButKeepsHyphenatedLineFragmentsTogether) {
  EXPECT_TRUE(HighlightSelection::quoteNeedsSeparator(12, 12, true, 'e'));
  EXPECT_FALSE(HighlightSelection::quoteNeedsSeparator(12, 12, true, '-'));
  EXPECT_FALSE(HighlightSelection::quoteNeedsSeparator(12, 12, false, 'e'));
  EXPECT_TRUE(HighlightSelection::quoteNeedsSeparator(12, 13, false, 'e'));
}
