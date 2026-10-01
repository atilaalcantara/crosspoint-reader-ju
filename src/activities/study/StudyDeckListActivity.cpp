#include "StudyDeckListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "StudySessionActivity.h"
#include "study/StudyStorage.h"

namespace fui = freeink::ui;
namespace {
constexpr size_t MAX_VISIBLE_DECKS = 512;

std::string packageLabel(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  return path.substr(start, (dot == std::string::npos || dot < start ? path.size() : dot) - start);
}
}  // namespace

StudyDeckListActivity::StudyDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("StudyDecks", renderer, mappedInput) {}

void StudyDeckListActivity::onEnter() {
  RenderLock lock(*this);
  closeRouting();
  tree_.clear();
  visible_.clear();
  rows_.clear();
  listItems_.clear();
  discoveryTruncated_ = false;
  truncatedCatalog_ = false;
  hadInvalidPackages_ = false;
  studyDirectoryReady_ = Study::ensureStudyDirectory();
  if (!studyDirectoryReady_) {
    listItems_.resize(1);
    listItems_[0].label = tr(STR_BACK);
    listItems_[0].actionValue = 0;
    UiListActivity::onEnter();
    return;
  }

  Study::Catalog catalog;
  const bool complete = Study::discoverFromSd(catalog);
  discoveryTruncated_ = !complete;
  for (const auto& file : catalog.files()) {
    if (!file.valid()) {
      hadInvalidPackages_ = true;
      continue;
    }
    tree_.addPackage(file.path, file.package.packageCrc(), file.package.decks());
  }
  rebuildRows();
  UiListActivity::onEnter();
}

void StudyDeckListActivity::rebuildRows() {
  visible_ = tree_.visibleRows(MAX_VISIBLE_DECKS, truncatedCatalog_);
  truncatedCatalog_ |= discoveryTruncated_;
  rows_.clear();
  rows_.reserve(visible_.size());
  for (const auto& visible : visible_) {
    const auto& package = tree_.package(visible.packageIndex);
    const auto& deck = tree_.deck(visible);
    const std::string indent(std::min<uint32_t>(visible.depth, 12) * 2, ' ');
    Row row;
    if (visible.kind == StudyDeckTree::RowKind::StudySubtree) {
      row.label = indent + tr(STR_STUDY);
    } else {
      row.label = indent + (tree_.hasChildren(visible) ? (deck.expanded ? "- " : "+ ") : "  ") + deck.info.name;
    }
    char count[24];
    snprintf(count, sizeof(count), "(%u)", static_cast<unsigned>(deck.info.subtreeCardCount));
    row.count = count;
    if (visible.packageHeading) row.packageHeading = packageLabel(package.path);
    rows_.push_back(std::move(row));
  }
  listItems_.resize(std::max<size_t>(rows_.size(), 1));
  if (rows_.empty()) {
    listItems_[0].label = tr(STR_BACK);
    listItems_[0].actionValue = 0;
  }
  for (size_t i = 0; i < rows_.size(); ++i) {
    listItems_[i].label = rows_[i].label.c_str();
    listItems_[i].value = rows_[i].count.c_str();
    listItems_[i].sectionHeading = rows_[i].packageHeading.empty() ? nullptr : rows_[i].packageHeading.c_str();
    listItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int StudyDeckListActivity::listCount() const { return rows_.empty() ? 1 : static_cast<int>(rows_.size()); }

const char* StudyDeckListActivity::headerTitle() const { return tr(STR_STUDY_DECKS); }

void StudyDeckListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  if (rows_.empty()) {
    const char* message = !studyDirectoryReady_ ? tr(STR_STUDY_SD_ERROR)
                          : hadInvalidPackages_  ? tr(STR_STUDY_INVALID_PACKAGE)
                                                 : tr(STR_STUDY_EMPTY);
    auto style = screen.theme().bodyText;
    style.align = fui::TextAlign::Center;
    style.maxLines = 5;
    screen.centeredText(message, style);
    fui::ListProps props;
    props.items = listItems_.data();
    props.count = 1;
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.rowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
    props.rowInset = 0;
    props.scrollIndicator = false;
    syncListViewport(screen, props);
    const int16_t band = static_cast<int16_t>(props.rowHeight + screen.theme().spaceSm);
    screen.list(props, band, fui::LayoutAnchor::Bottom);
    return;
  }
  if (truncatedCatalog_ || hadInvalidPackages_) {
    auto warning = screen.theme().smallText;
    warning.bold = true;
    screen.target().text(screen.takeTop(screen.target().lineHeight(warning.font), screen.theme().spaceXs),
                         truncatedCatalog_ ? tr(STR_STUDY_CATALOG_LIMIT) : tr(STR_STUDY_SKIPPED_INVALID), warning);
  }
  fui::ListProps props;
  props.items = listItems_.data();
  props.count = static_cast<uint16_t>(listItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 3;
  props.valueText = screen.theme().smallText;
  props.valueText.bold = true;
  props.headerText = screen.theme().smallText;
  props.headerText.bold = true;
  props.sectionGap = screen.theme().spaceXs;
  syncListViewport(screen, props);
  screen.list(props);
}

void StudyDeckListActivity::activateIndex(const int index) {
  if (index < 0) return;
  if (rows_.empty()) {
    finish();
    return;
  }
  if (static_cast<size_t>(index) >= rows_.size()) return;
  const auto selected = visible_[static_cast<size_t>(index)];
  if (selected.kind == StudyDeckTree::RowKind::Deck && tree_.hasChildren(selected)) {
    RenderLock lock(*this);
    closeRouting();
    tree_.toggle(selected);
    rebuildRows();
    nav.requestSelection(index);
    requestUpdate();
    return;
  }
  const auto& package = tree_.package(selected.packageIndex);
  app.clearTapFlash();
  auto session = makeUniqueNoThrow<StudySessionActivity>(renderer, mappedInput, package.path, package.crc,
                                                         tree_.deck(selected).info.id);
  if (!session) return;
  activityManager.pushActivity(std::move(session));
}
