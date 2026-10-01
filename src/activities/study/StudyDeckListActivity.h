#pragma once

#include <vector>

#include "activities/UiListActivity.h"
#include "StudyDeckTree.h"

class StudyDeckListActivity final : public UiListActivity {
 public:
  StudyDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

 private:
  struct Row {
    std::string label;
    std::string count;
    std::string packageHeading;
  };

  StudyDeckTree tree_;
  std::vector<StudyDeckTree::Row> visible_;
  std::vector<Row> rows_;
  std::vector<freeink::ui::ListItem> listItems_;
  bool hadInvalidPackages_ = false;
  bool discoveryTruncated_ = false;
  bool truncatedCatalog_ = false;
  bool studyDirectoryReady_ = true;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void rebuildRows();
};
