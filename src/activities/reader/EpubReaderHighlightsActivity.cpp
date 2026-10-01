#include "EpubReaderHighlightsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <numeric>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UiAppHelpers.h"
#include "components/UITheme.h"
#include "util/HighlightNavigation.h"
#include "util/HighlightNoteUpdate.h"
#include "util/HighlightStore.h"

namespace fui = freeink::ui;

EpubReaderHighlightsActivity::EpubReaderHighlightsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const std::shared_ptr<Epub>& epub,
                                                           std::vector<HighlightEntry>& highlights)
    : UiListActivity("EpubReaderHighlights", renderer, mappedInput, /*wantsTouchLongPress=*/true),
      epub(epub),
      highlights(&highlights) {
  rebuildRows();
}

void EpubReaderHighlightsActivity::rebuildRows() {
  readingOrder.clear();
  previews.clear();
  chapterTitles.clear();
  rowItems.clear();
  if (!highlights) return;

  readingOrder.resize(highlights->size());
  std::iota(readingOrder.begin(), readingOrder.end(), 0);
  std::sort(readingOrder.begin(), readingOrder.end(), [this](const size_t left, const size_t right) {
    return HighlightNavigation::comesBefore(highlights->at(left), highlights->at(right));
  });

  previews.reserve(readingOrder.size());
  chapterTitles.reserve(readingOrder.size());
  rowItems.reserve(readingOrder.size());
  for (const size_t index : readingOrder) {
    const auto& highlight = highlights->at(index);
    previews.push_back(HighlightNavigation::quotePreview(highlight.quote));

    std::string chapterTitle;
    if (epub && highlight.spineIndex < epub->getSpineItemsCount()) {
      const int tocIndex = epub->getTocIndexForSpineIndex(highlight.spineIndex);
      if (tocIndex >= 0) chapterTitle = epub->getTocItem(tocIndex).title;
    }
    if (!highlight.note.empty()) {
      if (!chapterTitle.empty()) chapterTitle += " · ";
      chapterTitle += tr(STR_NOTE);
    }
    chapterTitles.push_back(std::move(chapterTitle));

    fui::ListItem item;
    item.label = previews.back().c_str();
    item.subtitle = chapterTitles.back().empty() ? nullptr : chapterTitles.back().c_str();
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void EpubReaderHighlightsActivity::activateIndex(const int index) {
  if (!epub || !highlights || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;

  const auto& highlight = highlights->at(readingOrder[static_cast<size_t>(index)]);
  openHighlight(highlight.id);
}

void EpubReaderHighlightsActivity::openHighlight(const uint32_t highlightId) {
  if (!epub || !highlights) return;
  const auto it = std::find_if(highlights->begin(), highlights->end(),
                               [highlightId](const auto& highlight) { return highlight.id == highlightId; });
  if (it == highlights->end()) return;
  ProgressChangeResult location;
  location.spineIndex = it->spineIndex;
  location.hasVisibleTextOffset = true;
  location.visibleTextOffset = it->startOffset;
  setResult(std::move(location));
  finish();
}

void EpubReaderHighlightsActivity::onRowLongPress(const int index) {
  if (index < 0 || index >= listCount() || actionsPopup.isActive()) return;
  app.clearTapFlash();
  nav.selected = index;
  showActions(index);
}

void EpubReaderHighlightsActivity::showActions(const int index) {
  if (!highlights || index < 0 || index >= listCount() || actionsPopup.isActive()) return;
  const uint32_t id = highlights->at(readingOrder[static_cast<size_t>(index)]).id;
  const auto it = std::find_if(highlights->begin(), highlights->end(),
                               [id](const auto& highlight) { return highlight.id == id; });
  if (it == highlights->end()) return;

  const StrId options[] = {StrId::STR_OPEN, it->note.empty() ? StrId::STR_ADD_NOTE : StrId::STR_EDIT_NOTE,
                           StrId::STR_CANCEL};
  actionsPopupOpen = true;
  actionsPopup.show(StrId::STR_HIGHLIGHT_ACTIONS, options, 3, 0, [this, id](const int selected) {
    actionsPopupOpen = false;
    if (selected == 0) {
      openHighlight(id);
    } else if (selected == 1) {
      openNoteEditor(id);
    } else {
      requestUpdate();
    }
  });
  requestUpdate();
}

void EpubReaderHighlightsActivity::openNoteEditor(const uint32_t highlightId) {
  if (!epub || !highlights) return;
  const auto it = std::find_if(highlights->begin(), highlights->end(),
                               [highlightId](const auto& highlight) { return highlight.id == highlightId; });
  if (it == highlights->end()) return;
  const std::string initialNote = it->note;
  const StrId titleId = initialNote.empty() ? StrId::STR_ADD_NOTE : StrId::STR_EDIT_NOTE;
  auto editor = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(titleId), initialNote,
                                                        HighlightEntry::MAX_NOTE_BYTES, InputType::Text);
  if (!editor) {
    LOG_ERR("HLT", "OOM opening highlight note editor");
    return;
  }
  startActivityForResult(std::move(editor), [this, highlightId](const ActivityResult& result) {
    if (!highlights || !epub) return;
    bool shouldRequestUpdate = false;
    {
      // Row labels/items are referenced by the render task. Serialize the
      // collection save and row rebuild against that task's UiListActivity render.
      RenderLock lock;
      const auto it = std::find_if(highlights->begin(), highlights->end(),
                                   [highlightId](const auto& highlight) { return highlight.id == highlightId; });
      if (it == highlights->end()) return;
      std::string replacement;
      if (!result.isCancelled) replacement = std::get<KeyboardResult>(result.data).text;
      const auto update = HighlightNoteUpdate::apply(*it, result.isCancelled, std::move(replacement), [this] {
        return HighlightStore::save(epub->getPath(), *highlights);
      });
      if (update == HighlightNoteUpdate::Result::Cancelled) return;
      if (update == HighlightNoteUpdate::Result::Invalid) {
        pendingNoteSaveError = true;
        shouldRequestUpdate = true;
        LOG_ERR("HLT", "Rejected invalid note for highlight %u", highlightId);
      } else if (update == HighlightNoteUpdate::Result::SaveFailed) {
        pendingNoteSaveError = true;
        shouldRequestUpdate = true;
        LOG_ERR("HLT", "Failed to persist note for highlight %u", highlightId);
      } else {
        rebuildRows();
        shouldRequestUpdate = true;
      }
    }
    if (shouldRequestUpdate) requestUpdate();
  });
}

bool EpubReaderHighlightsActivity::handleCustomInput() {
  if (actionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (actionsPopupOpen) {
    // Back/outside dismissal does not invoke the popup's selection callback.
    actionsPopupOpen = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool EpubReaderHighlightsActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 700)) {
    showActions(nav.selected);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }
  return false;
}

void EpubReaderHighlightsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  if (pendingNoteSaveError) {
    pendingNoteSaveError = false;
    GUI.drawPopup(renderer, tr(STR_NOTE_SAVE_FAILED));
  }
  if (actionsPopup.processRender(renderer, mappedInput)) return;
  drawFooter();
  renderer.displayBuffer();
}

void EpubReaderHighlightsActivity::buildScreen(UiScreen& screen) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (readingOrder.empty()) {
    screen.centeredText(tr(STR_NO_HIGHLIGHTS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
