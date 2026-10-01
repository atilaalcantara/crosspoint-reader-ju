#include <gtest/gtest.h>

#include "activities/study/StudyDeckTree.h"
#include "../support/StudyFixtureBuilder.h"

namespace {
Study::Package openFixture(StudyMemoryFile& source) {
  source.bytes = makeStudyFixture({{4, 2, "grandchild"}, {2, 1, "child"}, {5, 0, "other"},
                                   {1, 0, "root"}, {3, 1, "sibling"}},
                                  {{10, 4, "front", "back"}, {11, 3, "front", "back"}});
  Study::Package package;
  Study::Error error;
  EXPECT_TRUE(package.open(source, error)) << Study::errorName(error);
  return package;
}
}  // namespace

TEST(StudyDeckTree, ExpandsDepthFirstWithStudyActionAndScopedPackages) {
  StudyMemoryFile file;
  const auto package = openFixture(file);
  StudyDeckTree tree;
  tree.addPackage("/study/first.cps", package.packageCrc(), package.decks());
  tree.addPackage("/study/second.cps", 17, package.decks());
  bool truncated = false;
  auto rows = tree.visibleRows(512, truncated);
  ASSERT_EQ(rows.size(), 4u);  // two roots per package, initially collapsed
  EXPECT_FALSE(truncated);
  EXPECT_EQ(tree.deck(rows[0]).info.id, 5u);
  EXPECT_EQ(tree.deck(rows[1]).info.id, 1u);
  EXPECT_TRUE(rows[0].packageHeading);
  EXPECT_TRUE(rows[2].packageHeading);

  tree.toggle(rows[1]);
  rows = tree.visibleRows(512, truncated);
  ASSERT_EQ(rows.size(), 7u);
  EXPECT_EQ(rows[2].kind, StudyDeckTree::RowKind::StudySubtree);
  EXPECT_EQ(tree.deck(rows[2]).info.id, 1u);
  EXPECT_EQ(tree.deck(rows[2]).info.subtreeCardCount, 2u);
  EXPECT_EQ(tree.deck(rows[3]).info.id, 2u);
  EXPECT_EQ(rows[3].depth, 1u);
  EXPECT_EQ(tree.deck(rows[4]).info.id, 3u);
  EXPECT_EQ(tree.deck(rows[5]).info.id, 5u);  // second package remains collapsed

  tree.toggle(rows[3]);
  rows = tree.visibleRows(512, truncated);
  ASSERT_EQ(rows.size(), 9u);
  EXPECT_EQ(rows[4].kind, StudyDeckTree::RowKind::StudySubtree);
  EXPECT_EQ(tree.deck(rows[4]).info.id, 2u);
  EXPECT_EQ(tree.deck(rows[5]).info.id, 4u);
  EXPECT_EQ(rows[5].depth, 2u);
  tree.toggle(rows[1]);
  rows = tree.visibleRows(512, truncated);
  EXPECT_EQ(rows.size(), 4u);
  tree.toggle(rows[1]);
  rows = tree.visibleRows(512, truncated);
  EXPECT_EQ(rows.size(), 9u);  // nested expansion survives parent collapse
  EXPECT_EQ(tree.package(rows[7].packageIndex).path, "/study/second.cps");
}

TEST(StudyDeckTree, LimitAppliesToVisibleRowsOnly) {
  StudyMemoryFile file;
  const auto package = openFixture(file);
  StudyDeckTree tree;
  tree.addPackage("/study/first.cps", package.packageCrc(), package.decks());
  bool truncated = false;
  auto rows = tree.visibleRows(2, truncated);
  EXPECT_EQ(rows.size(), 2u);
  EXPECT_FALSE(truncated);
  tree.toggle(rows[1]);
  rows = tree.visibleRows(2, truncated);
  EXPECT_EQ(rows.size(), 2u);
  EXPECT_TRUE(truncated);
}
