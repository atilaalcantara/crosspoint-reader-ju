#pragma once

#include <string>
#include <vector>

#include "HighlightEntry.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// The book rows are derived from sidecars, not the recent-books window.
class GlobalHighlightsActivity final : public UiListActivity {
 public:
  GlobalHighlightsActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;

 private:
  struct Book {
    std::string path;
    std::vector<HighlightEntry> entries;
  };
  std::vector<Book> books;
  std::vector<size_t> order;
  std::vector<std::string> labels;
  std::vector<std::string> subtitles;
  std::vector<freeink::ui::ListItem> rows;
  size_t selectedBook = 0;
  bool showingBook = false;
  bool loadError = false;
  bool saveError = false;
  bool noteError = false;
  OptionPopup popup;
  bool popupOpen = false;

  void loadBooks();
  void rebuildRows();
  void showActions(int index);
  void openHighlight(uint32_t id);
  void editNote(uint32_t id);
  void confirmRemove(uint32_t id);
  void removeHighlight(uint32_t id);
  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  void render(RenderLock&& lock) override;
  const char* headerTitle() const override;
};
