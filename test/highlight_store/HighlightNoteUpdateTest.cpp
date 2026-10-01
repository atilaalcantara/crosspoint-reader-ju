#include <util/HighlightNoteUpdate.h>
#include <gtest/gtest.h>

#include <string>

TEST(HighlightNoteUpdateTest, CancelLeavesTheOriginalNoteAndDoesNotWrite) {
  HighlightEntry highlight{1, 0, 10, 15, "quote", "existing"};
  bool wrote = false;
  const auto result = HighlightNoteUpdate::apply(highlight, true, "cancelled edit", [&] {
    wrote = true;
    return true;
  });
  EXPECT_EQ(result, HighlightNoteUpdate::Result::Cancelled);
  EXPECT_EQ(highlight.note, "existing");
  EXPECT_FALSE(wrote);
}

TEST(HighlightNoteUpdateTest, SavesUnicodeAndEmptyNoteThroughTheSameUpdatePath) {
  HighlightEntry highlight{1, 0, 10, 15, "quote", "old"};
  auto result = HighlightNoteUpdate::apply(highlight, false, "ação 🐟", [] { return true; });
  EXPECT_EQ(result, HighlightNoteUpdate::Result::Saved);
  EXPECT_EQ(highlight.note, "ação 🐟");

  result = HighlightNoteUpdate::apply(highlight, false, "", [] { return true; });
  EXPECT_EQ(result, HighlightNoteUpdate::Result::Saved);
  EXPECT_TRUE(highlight.note.empty());
  EXPECT_TRUE(highlight.isValid());
}

TEST(HighlightNoteUpdateTest, FailedWriteRollsBackAndInvalidNotesNeverWrite) {
  HighlightEntry highlight{1, 0, 10, 15, "quote", "keep"};
  EXPECT_EQ(HighlightNoteUpdate::apply(highlight, false, "not saved", [] { return false; }),
            HighlightNoteUpdate::Result::SaveFailed);
  EXPECT_EQ(highlight.note, "keep");

  bool wrote = false;
  EXPECT_EQ(HighlightNoteUpdate::apply(highlight, false, std::string(HighlightEntry::MAX_NOTE_BYTES + 1, 'x'), [&] {
             wrote = true;
             return true;
           }),
           HighlightNoteUpdate::Result::Invalid);
  EXPECT_FALSE(wrote);
  EXPECT_EQ(HighlightNoteUpdate::apply(highlight, false, std::string("\xF0\x9F", 2), [] { return true; }),
            HighlightNoteUpdate::Result::Invalid);
  EXPECT_EQ(highlight.note, "keep");
}
