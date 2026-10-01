#pragma once

#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "../../HighlightEntry.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderHighlightsActivity final : public UiListActivity {
 public:
  EpubReaderHighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::shared_ptr<Epub>& epub,
                               std::vector<HighlightEntry>& highlights);

 private:
  std::shared_ptr<Epub> epub;
  // EpubReaderActivity remains on the activity stack for this child screen, so
  // the source collection stays alive without copying every quote into SRAM.
  std::vector<HighlightEntry>* highlights = nullptr;
  std::vector<size_t> readingOrder;
  std::vector<std::string> previews;
  std::vector<std::string> chapterTitles;
  std::vector<freeink::ui::ListItem> rowItems;
  OptionPopup actionsPopup;
  bool actionsPopupOpen = false;
  bool pendingNoteSaveError = false;

  int listCount() const override { return static_cast<int>(readingOrder.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override { return tr(STR_HIGHLIGHTS); }
  void render(RenderLock&& lock) override;
  void rebuildRows();
  void showActions(int index);
  void openHighlight(uint32_t highlightId);
  void openNoteEditor(uint32_t highlightId);
};
