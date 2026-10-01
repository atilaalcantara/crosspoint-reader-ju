#include "GlobalHighlightsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <numeric>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/HighlightNavigation.h"
#include "util/HighlightNoteUpdate.h"
#include "util/HighlightStore.h"

namespace fui = freeink::ui;

GlobalHighlightsActivity::GlobalHighlightsActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("GlobalHighlights", renderer, input, true) {}

void GlobalHighlightsActivity::onEnter() {
  loadBooks();
  UiListActivity::onEnter();
}

void GlobalHighlightsActivity::loadBooks() {
  std::vector<std::string> paths;
  loadError = !HighlightStore::listBookPaths(paths);
  books.clear();
  for (auto& path : paths) {
    Book book;
    book.path = std::move(path);
    books.push_back(std::move(book));
  }
  rebuildRows();
}

void GlobalHighlightsActivity::rebuildRows() {
  order.clear();
  labels.clear();
  subtitles.clear();
  rows.clear();
  if (showingBook && selectedBook >= books.size()) showingBook = false;
  if (showingBook) {
    const auto& entries = books[selectedBook].entries;
    order.resize(entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      return HighlightNavigation::comesBefore(entries[a], entries[b]);
    });
    labels.reserve(order.size());
    subtitles.reserve(order.size());
    for (size_t i : order) {
      labels.push_back(HighlightNavigation::quotePreview(entries[i].quote));
      subtitles.push_back(entries[i].note.empty() ? "" : tr(STR_NOTE));
    }
  } else {
    labels.reserve(books.size());
    subtitles.reserve(books.size());
    for (const auto& book : books) {
      const auto slash = book.path.find_last_of('/');
      labels.push_back(book.path.substr(slash == std::string::npos ? 0 : slash + 1));
      subtitles.push_back(book.path);
    }
  }
  rows.reserve(labels.size());
  for (size_t i = 0; i < labels.size(); ++i) {
    fui::ListItem item;
    item.label = labels[i].c_str();
    item.subtitle = subtitles[i].empty() ? nullptr : subtitles[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    rows.push_back(item);
  }
  nav.selected = 0;
  nav.top = 0;
}

void GlobalHighlightsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (!showingBook) {
    const size_t bookIndex = static_cast<size_t>(index);
    std::vector<HighlightEntry> entries;
    if (!HighlightStore::load(books[bookIndex].path, entries)) {
      loadError = true;
      requestUpdate();
      return;
    }
    RenderLock lock;
    selectedBook = bookIndex;
    books[selectedBook].entries = std::move(entries);
    showingBook = true;
    rebuildRows();
    requestUpdate();
    return;
  }
  showActions(index);
}

void GlobalHighlightsActivity::onRowLongPress(int index) { activateIndex(index); }

void GlobalHighlightsActivity::showActions(int index) {
  if (!showingBook || index < 0 || index >= listCount() || popup.isActive()) return;
  nav.selected = index;
  const auto& entry = books[selectedBook].entries[order[static_cast<size_t>(index)]];
  const uint32_t id = entry.id;
  const StrId options[] = {StrId::STR_OPEN, entry.note.empty() ? StrId::STR_ADD_NOTE : StrId::STR_EDIT_NOTE,
                           StrId::STR_DELETE, StrId::STR_CANCEL};
  popupOpen = true;
  popup.show(StrId::STR_HIGHLIGHT_ACTIONS, options, 4, 0, [this, id](int choice) {
    popupOpen = false;
    if (choice == 0) openHighlight(id);
    else if (choice == 1) editNote(id);
    else if (choice == 2) confirmRemove(id);
    else requestUpdate();
  });
  requestUpdate();
}

void GlobalHighlightsActivity::openHighlight(uint32_t id) {
  const Book& book = books[selectedBook];
  const auto it = std::find_if(book.entries.begin(), book.entries.end(),
                               [id](const auto& entry) { return entry.id == id; });
  if (it == book.entries.end()) return;
  if (!Storage.exists(book.path.c_str())) {
    loadError = true;
    requestUpdate();
    return;
  }
  activityManager.goToHighlightedText(book.path, it->spineIndex, it->startOffset);
}

void GlobalHighlightsActivity::editNote(uint32_t id) {
  Book& book = books[selectedBook];
  const auto it = std::find_if(book.entries.begin(), book.entries.end(),
                               [id](const auto& entry) { return entry.id == id; });
  if (it == book.entries.end()) return;
  const StrId title = it->note.empty() ? StrId::STR_ADD_NOTE : StrId::STR_EDIT_NOTE;
  auto editor = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(title), it->note,
                                                          HighlightEntry::MAX_NOTE_BYTES, InputType::Text);
  if (!editor) return;
  startActivityForResult(std::move(editor), [this, id](const ActivityResult& result) {
    if (result.isCancelled) return;
    bool refresh = false;
    {
      RenderLock lock;
      Book& book = books[selectedBook];
      const auto it = std::find_if(book.entries.begin(), book.entries.end(),
                                   [id](const auto& entry) { return entry.id == id; });
      if (it == book.entries.end()) return;
      const auto status = HighlightNoteUpdate::apply(*it, false, std::get<KeyboardResult>(result.data).text,
                                                      [&] { return HighlightStore::save(book.path, book.entries); });
      if (status == HighlightNoteUpdate::Result::Saved) rebuildRows();
      else noteError = true;
      refresh = true;
    }
    if (refresh) requestUpdate();
  });
}

void GlobalHighlightsActivity::confirmRemove(uint32_t id) {
  const StrId options[] = {StrId::STR_DELETE, StrId::STR_CANCEL};
  popupOpen = true;
  popup.show(StrId::STR_DELETE, options, 2, 1, [this, id](int choice) {
    popupOpen = false;
    if (choice == 0) removeHighlight(id);
    else requestUpdate();
  });
  requestUpdate();
}

void GlobalHighlightsActivity::removeHighlight(uint32_t id) {
  {
    RenderLock lock;
    Book& book = books[selectedBook];
    const auto it = std::find_if(book.entries.begin(), book.entries.end(),
                                 [id](const auto& entry) { return entry.id == id; });
    if (it == book.entries.end()) return;
    const size_t index = static_cast<size_t>(it - book.entries.begin());
    HighlightEntry removed = *it;
    book.entries.erase(it);
    if (!HighlightStore::save(book.path, book.entries)) {
      book.entries.insert(book.entries.begin() + static_cast<std::ptrdiff_t>(index), std::move(removed));
      saveError = true;
    } else if (book.entries.empty()) {
      books.erase(books.begin() + static_cast<std::ptrdiff_t>(selectedBook));
    }
    rebuildRows();
  }
  requestUpdate();
}

bool GlobalHighlightsActivity::handleCustomInput() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (popupOpen) {
    popupOpen = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool GlobalHighlightsActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateIndex(nav.selected);
    return true;
  }
  return false;
}

void GlobalHighlightsActivity::onBackButton() {
  if (!showingBook) {
    finish();
    return;
  }
  {
    RenderLock lock;
    books[selectedBook].entries.clear();
    showingBook = false;
    rebuildRows();
  }
  requestUpdate();
}

const char* GlobalHighlightsActivity::headerTitle() const { return tr(STR_HIGHLIGHTS); }

void GlobalHighlightsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  drawChrome();
  renderUi();
  if (loadError || saveError || noteError) {
    GUI.drawPopup(renderer, noteError ? tr(STR_NOTE_SAVE_FAILED)
                                      : saveError ? tr(STR_HIGHLIGHT_SAVE_FAILED) : tr(STR_HIGHLIGHTS_LOAD_FAILED));
    loadError = false;
    saveError = false;
    noteError = false;
  }
  if (popup.processRender(renderer, mappedInput)) return;
  drawFooter();
  renderer.displayBuffer();
}

void GlobalHighlightsActivity::buildScreen(UiScreen& screen) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (rows.empty()) {
    screen.centeredText(showingBook ? tr(STR_NO_HIGHLIGHTS) : tr(STR_NO_SAVED_HIGHLIGHTS), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
