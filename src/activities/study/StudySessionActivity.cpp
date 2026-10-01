#include "StudySessionActivity.h"

#include <Epub/converters/PngToFramebufferConverter.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "components/UITheme.h"

namespace fui = freeink::ui;
namespace {
constexpr uint32_t NO_CARD = 0xFFFFFFFFu;
constexpr size_t MAX_DISPLAY_LINE_BYTES = 200;

size_t utf8SequenceBytes(const std::string& text, const size_t offset) {
  const auto lead = static_cast<uint8_t>(text[offset]);
  if (lead <= 0x7F) return 1;

  size_t length = 0;
  uint32_t codepoint = 0;
  uint32_t minimum = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    codepoint = lead & 0x1F;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    codepoint = lead & 0x0F;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    codepoint = lead & 0x07;
    minimum = 0x10000;
  } else {
    return 1;
  }
  if (length > text.size() - offset) return 1;
  for (size_t i = 1; i < length; ++i) {
    const auto continuation = static_cast<uint8_t>(text[offset + i]);
    if ((continuation & 0xC0) != 0x80) return 1;
    codepoint = (codepoint << 6) | (continuation & 0x3F);
  }
  if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return 1;
  return length;
}

bool displayTextNeedsNormalization(const std::string& text) {
  constexpr char EMOJI_SELECTOR[] = "\xEF\xB8\x8F";
  if (text.find(EMOJI_SELECTOR) != std::string::npos || text.find("\n\n\n") != std::string::npos) return true;
  size_t lineBytes = 0;
  for (size_t offset = 0; offset < text.size();) {
    if (text[offset] == '\n') {
      lineBytes = 0;
      ++offset;
      continue;
    }
    const size_t codepointBytes = utf8SequenceBytes(text, offset);
    if (lineBytes > 0 && lineBytes + codepointBytes > MAX_DISPLAY_LINE_BYTES) return true;
    lineBytes += codepointBytes;
    offset += codepointBytes;
  }
  return false;
}

std::string normalizeDisplayText(const std::string& text) {
  // The package bytes stay intact. Study ignores emoji presentation and extra
  // template spacing in its display copy, and bounds unbroken runs for wrapping.
  std::string displayText;
  displayText.reserve(text.size());
  size_t lineBytes = 0;
  for (size_t offset = 0; offset < text.size();) {
    const auto byte = static_cast<uint8_t>(text[offset]);
    if (byte == 0xEF && offset + 2 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0xB8 &&
        static_cast<uint8_t>(text[offset + 2]) == 0x8F) {
      offset += 3;
      continue;
    }
    if (text[offset] == '\n') {
      size_t runEnd = offset + 1;
      while (runEnd < text.size() && text[runEnd] == '\n') ++runEnd;
      displayText.append(text, offset, std::min<size_t>(runEnd - offset, 2));
      lineBytes = 0;
      offset = runEnd;
      continue;
    }
    const size_t codepointBytes = utf8SequenceBytes(text, offset);
    if (lineBytes > 0 && lineBytes + codepointBytes > MAX_DISPLAY_LINE_BYTES) {
      displayText.push_back('\n');
      lineBytes = 0;
    }
    displayText.append(text, offset, codepointBytes);
    lineBytes += codepointBytes;
    offset += codepointBytes;
  }
  return displayText;
}

int32_t currentDay() {
  const time_t now = time(nullptr);
  if (now <= 0) return 0;
  const int64_t day = static_cast<int64_t>(now) / 86400;
  return day > INT32_MAX ? INT32_MAX : static_cast<int32_t>(day);
}
}  // namespace

StudySessionActivity::StudySessionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string packagePath, const uint32_t packageCrc,
                                           const uint32_t deckId)
    : UiListActivity("StudySession", renderer, mappedInput), packagePath_(std::move(packagePath)),
      expectedPackageCrc_(packageCrc), deckId_(deckId), packageAccess_(packageFile_) {}

void StudySessionActivity::onEnter() {
  RenderLock lock(*this);
  closeRouting();
  if (!loadSession()) {
    UiListActivity::onEnter();
    return;
  }
  UiListActivity::onEnter();
}

void StudySessionActivity::onExit() {
  persistent_.close();
  // Guard every handle: loadSession can fail before any file is opened, and
  // HalFile::close() asserts on a never-opened file (hardware crash screen).
  if (mediaFile_.isOpen()) mediaFile_.close();
  mediaIndex_.reset();
  mediaChecked_ = false;
  mediaDirectory_.clear();
  frontImages_ = {};
  backImages_ = {};
  frontDimsProbed_ = {};
  backDimsProbed_ = {};
  if (packageFile_.isOpen()) packageFile_.close();
  opened_ = false;
  UiListActivity::onExit();
}

void StudySessionActivity::onBackButton() { finish(); }

bool StudySessionActivity::handleCustomInput() {
  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::Up && swipe != MappedInputManager::SwipeDir::Down) {
    return false;
  }

  RenderLock lock(*this);
  if (view_ != View::Front && view_ != View::Back) return false;
  const uint32_t maximum = maxTextTopLine_.load();
  if (maximum == 0) return false;
  const uint32_t current = textTopLine_.load();
  const uint32_t visible = visibleTextLines_.load();
  const uint32_t step = visible > 1 ? visible - 1 : 1;
  uint32_t next = current;
  if (swipe == MappedInputManager::SwipeDir::Up) {
    next = current + step > maximum ? maximum : current + step;
  } else {
    next = current > step ? current - step : 0;
  }
  if (next == current) return true;
  textTopLine_.store(next);
  requestUpdate();
  return true;
}

int StudySessionActivity::listCount() const {
  if (view_ == View::Front || view_ == View::Empty || view_ == View::Error) return 1;
  if (view_ == View::Back) return 4;
  return 2;
}

const char* StudySessionActivity::headerTitle() const { return tr(STR_STUDY); }

bool StudySessionActivity::loadSession() {
  today_ = currentDay();
  packageFile_ = Storage.open(packagePath_.c_str(), O_RDONLY);
  if (!packageFile_) {
    errorMessage_ = tr(STR_STUDY_OPEN_ERROR);
    view_ = View::Error;
    return false;
  }
  Study::Error error;
  if (!package_.open(packageAccess_, error) || package_.packageCrc() != expectedPackageCrc_) {
    errorMessage_ = package_.packageCrc() != expectedPackageCrc_ ? tr(STR_STUDY_PACKAGE_CHANGED)
                                                                  : tr(STR_STUDY_OPEN_ERROR);
    view_ = View::Error;
    return false;
  }
  const Study::DeckInfo* deck = package_.deck(deckId_);
  if (!deck) {
    errorMessage_ = tr(STR_STUDY_OPEN_ERROR);
    view_ = View::Error;
    return false;
  }
  if (deck->subtreeCardCount == 0) {
    view_ = View::Empty;
    return true;
  }
  if (!persistent_.open(packagePath_, package_.packageCrc(), package_.cardCount(), error)) {
    errorMessage_ = error == Study::Error::InvalidHeader ? tr(STR_STUDY_PACKAGE_CHANGED)
                                                         : tr(STR_STUDY_INVALID_STATUS);
    view_ = View::Error;
    return false;
  }

  checkpoint_.packageCrc = package_.packageCrc();
  checkpoint_.cardCount = package_.cardCount();
  checkpoint_.deckId = deckId_;
  checkpoint_.phase = Study::SessionPhase::Main;
  checkpoint_.nextIndex = 0;
  checkpoint_.currentIndex = NO_CARD;
  checkpoint_.currentCardId = 0;
  Study::SessionCheckpoint saved;
  bool restore = Study::loadCheckpoint(packagePath_, package_.packageCrc(), package_.cardCount(), saved) &&
                 saved.deckId == deckId_;
  if (restore && saved.currentIndex != NO_CARD) {
    uint64_t actualId = 0;
    if (!package_.cardBelongsToDeck(saved.currentIndex, deckId_, true)) {
      restore = false;
    } else if (!package_.cardIdAt(packageAccess_, saved.currentIndex, actualId, error)) {
      errorMessage_ = tr(STR_STUDY_READ_ERROR);
      view_ = View::Error;
      return false;
    } else if (actualId != saved.currentCardId) {
      restore = false;
    } else {
      Study::CardState savedState;
      if (!persistent_.load(saved.currentIndex, actualId, savedState, error)) {
        errorMessage_ = tr(STR_STUDY_INVALID_STATUS);
        view_ = View::Error;
        return false;
      }
      if (!Study::cardStateEligible(savedState, saved.phase, today_)) restore = false;
    }
  }
  if (restore) {
    checkpoint_ = saved;
  }

  opened_ = true;
  if (checkpoint_.phase == Study::SessionPhase::Done) {
    checkpoint_.phase = Study::SessionPhase::Main;
    checkpoint_.nextIndex = 0;
    checkpoint_.currentIndex = NO_CARD;
    checkpoint_.currentCardId = 0;
    return findAndLoadNext();
  }
  if (checkpoint_.currentIndex != NO_CARD) {
    if (!loadCurrent(checkpoint_.currentIndex, checkpoint_.currentCardId, checkpoint_.face)) return false;
    return true;
  }
  return findAndLoadNext();
}

bool StudySessionActivity::lookupState(void* context, const uint32_t index, const uint64_t cardId,
                                       Study::CardState& state, Study::Error& error) {
  return static_cast<StudySessionActivity*>(context)->persistent_.load(index, cardId, state, error);
}

bool StudySessionActivity::findAndLoadNext() {
  Study::Error error;
  int32_t index = Study::findNextCard(package_, packageAccess_, deckId_, true, checkpoint_.phase,
                                      checkpoint_.nextIndex, today_, &StudySessionActivity::lookupState, this, error);
  if (index < 0 && error != Study::Error::None) {
    errorMessage_ = error == Study::Error::InvalidChecksum || error == Study::Error::InvalidCard
                        ? tr(STR_STUDY_INVALID_STATUS)
                        : tr(STR_STUDY_READ_ERROR);
    view_ = View::Error;
    return false;
  }
  if (index < 0 &&
      (checkpoint_.phase == Study::SessionPhase::Main || checkpoint_.phase == Study::SessionPhase::Cram)) {
    checkpoint_.phase = Study::SessionPhase::Again;
    checkpoint_.nextIndex = 0;
    index = Study::findNextCard(package_, packageAccess_, deckId_, true, checkpoint_.phase, 0, today_,
                                &StudySessionActivity::lookupState, this, error);
  }
  if (index < 0) {
    if (error != Study::Error::None) {
      errorMessage_ = tr(STR_STUDY_READ_ERROR);
      view_ = View::Error;
      return false;
    }
    checkpoint_.phase = Study::SessionPhase::Done;
    checkpoint_.nextIndex = package_.cardCount();
    checkpoint_.currentIndex = NO_CARD;
    checkpoint_.currentCardId = 0;
    checkpoint_.face = Study::CardFace::Front;
    saveCheckpoint();
    view_ = View::Done;
    return true;
  }
  uint64_t id = 0;
  if (!package_.cardIdAt(packageAccess_, static_cast<uint32_t>(index), id, error) ||
      !loadCurrent(static_cast<uint32_t>(index), id, Study::CardFace::Front)) {
    errorMessage_ = tr(STR_STUDY_READ_ERROR);
    view_ = View::Error;
    return false;
  }
  return true;
}

bool StudySessionActivity::loadCurrent(const uint32_t index, const uint64_t expectedId, const Study::CardFace face) {
  Study::Error error;
  Study::Card loaded;
  Study::CardState loadedState;
  if (!package_.cardBelongsToDeck(index, deckId_, true) ||
      !package_.loadCard(packageAccess_, index, loaded, error) || loaded.id != expectedId ||
      !persistent_.load(index, expectedId, loadedState, error)) {
    errorMessage_ = error == Study::Error::InvalidChecksum || error == Study::Error::InvalidCard
                        ? tr(STR_STUDY_INVALID_STATUS)
                        : tr(STR_STUDY_READ_ERROR);
    view_ = View::Error;
    return false;
  }
  card_ = std::move(loaded);
  frontImages_ = {};
  backImages_ = {};
  frontDimsProbed_ = {};
  backDimsProbed_ = {};
  frontDimsOk_ = {};
  backDimsOk_ = {};
  if (!mediaChecked_) {
    mediaChecked_ = true;
    mediaDirectory_ = Study::MediaIndex::directoryFor(packagePath_);
    if (!mediaDirectory_.empty() &&
        Storage.openFileForRead("STUDY", mediaDirectory_ + "/index.bin", mediaFile_) &&
        !mediaIndex_.open(mediaAccess_, package_.packageCrc())) {
      mediaFile_.close();
    }
  }
  if (mediaFile_ && !mediaIndex_.imagesFor(mediaAccess_, card_.id, frontImages_, backImages_)) mediaFile_.close();
  backStartsWithFront_ = !card_.front.empty() && card_.back.size() >= card_.front.size() &&
                         card_.back.compare(0, card_.front.size(), card_.front) == 0;
  frontNeedsDisplayNormalization_ = displayTextNeedsNormalization(card_.front);
  backNeedsDisplayNormalization_ = displayTextNeedsNormalization(card_.back);
  frontDisplayText_.clear();
  backDisplayText_.clear();
  if (frontNeedsDisplayNormalization_) frontDisplayText_ = normalizeDisplayText(card_.front);
  if (backNeedsDisplayNormalization_) backDisplayText_ = normalizeDisplayText(card_.back);
  textTopLine_.store(0);
  maxTextTopLine_.store(0);
  visibleTextLines_.store(0);
  cardState_ = loadedState;
  ++layoutGeneration_;
  layoutCache_ = {};
  ratingValid_ = {};
  for (size_t i = 0; i < ratingIntervals_.size(); ++i) {
    ratingIntervals_[i][0] = '\0';
    Study::CardState preview = cardState_;
    if (!Study::applyRating(preview, static_cast<Study::Rating>(i + 1), today_,
                            checkpoint_.phase == Study::SessionPhase::Again))
      continue;
    ratingValid_[i] = true;
    if (preview.status == Study::CardStatus::Learning) {
      std::snprintf(ratingIntervals_[i].data(), ratingIntervals_[i].size(), "%s", tr(STR_STUDY_THIS_SESSION));
    } else {
      const char* intervalFormat = preview.intervalDays == 1 ? tr(STR_STUDY_DAY_SHORT) : tr(STR_STUDY_DAYS_SHORT);
      std::snprintf(ratingIntervals_[i].data(), ratingIntervals_[i].size(), intervalFormat,
                    static_cast<unsigned long>(preview.intervalDays));
    }
  }
  checkpoint_.currentIndex = index;
  checkpoint_.currentCardId = expectedId;
  checkpoint_.face = face;
  checkpoint_.nextIndex = index + 1;
  view_ = face == Study::CardFace::Front ? View::Front : View::Back;
  saveCheckpoint();
  return true;
}

int16_t StudySessionActivity::actionBandHeight(UiScreen& screen, const int count) const {
  const auto& theme = screen.theme();
  if (view_ == View::Back) {
    const int16_t tileHeight = std::max<int16_t>(64, theme.rowHeight);
    return static_cast<int16_t>(tileHeight * 2 + theme.spaceSm * 2 + screen.target().lineHeight(theme.smallText.font));
  }
  int16_t rowHeight = theme.rowHeight;
  if (!mappedInput.hasTouch() || rowHeight <= 0)
    rowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
  const int16_t gaps = count > 1 ? static_cast<int16_t>(theme.listRowGap * (count - 1)) : 0;
  return static_cast<int16_t>(rowHeight * count + gaps + theme.spaceSm);
}

uint32_t StudySessionActivity::drawCardText(UiScreen& screen, const std::string& text, const int16_t height,
                                                const int cacheSlot) {
  if (height <= 0) {
    maxTextTopLine_.store(0);
    visibleTextLines_.store(0);
    return 0;
  }

  const fui::Rect viewport = screen.takeTop(height, screen.theme().spaceSm);
  auto style = screen.theme().bodyText;
  style.maxLines = 1;
  const int16_t lineHeight = screen.target().lineHeight(style.font);
  if (lineHeight <= 0 || viewport.width <= 0) {
    maxTextTopLine_.store(0);
    visibleTextLines_.store(0);
    return 0;
  }

  // Cached per card face: the line count and narrowed text width depend only
  // on (card, viewport width). Scroll frames otherwise pay two full
  // text-measurement walks before drawing anything.
  uint32_t lineCount = 0;
  int16_t textWidth = viewport.width;
  auto& cache = layoutCache_[cacheSlot >= 0 && cacheSlot < 2 ? cacheSlot : 0];
  if (cache.valid && cache.generation == layoutGeneration_ && cache.viewportWidth == viewport.width) {
    lineCount = cache.lines;
    textWidth = cache.textWidth;
  } else {
    lineCount = fui::textAreaWalk(screen.target(), viewport.width, text.c_str(), style,
                                  [](uint32_t, const fui::TextAreaLine&) {});
    if (lineCount > fui::textAreaVisibleLines(viewport, lineHeight) && viewport.width > 8) {
      textWidth = static_cast<int16_t>(viewport.width - 6);
      lineCount = fui::textAreaWalk(screen.target(), textWidth, text.c_str(), style,
                                    [](uint32_t, const fui::TextAreaLine&) {});
    }
    cache = {layoutGeneration_, viewport.width, textWidth, lineCount, true};
  }
  const uint16_t visible = fui::textAreaVisibleLines(viewport, lineHeight);
  if (visible == 0) {
    maxTextTopLine_.store(0);
    visibleTextLines_.store(0);
    return lineCount;
  }

  const uint32_t maxTop = lineCount > visible ? lineCount - visible : 0;
  uint32_t top = std::min(textTopLine_.load(), maxTop);
  textTopLine_.store(top);
  maxTextTopLine_.store(maxTop);
  visibleTextLines_.store(visible);

  fui::textAreaWalk(screen.target(), textWidth, text.c_str(), style,
                    [&](const uint32_t lineIndex, const fui::TextAreaLine& line) {
                      if (lineIndex < top || lineIndex >= top + visible) return;
                      textLineBuffer_.assign(text.data() + line.start, line.len);
                      const int16_t y = static_cast<int16_t>(viewport.y + (lineIndex - top) * lineHeight);
                      screen.target().text(fui::Rect{viewport.x, y, textWidth, lineHeight}, textLineBuffer_.c_str(),
                                           style);
                    });
  if (maxTop > 0) fui::drawListScrollIndicator(screen.target(), viewport, lineCount, visible, top, 2);
  return lineCount;
}

int16_t StudySessionActivity::drawCardImages(UiScreen& screen, const Study::MediaIndex::Names& images,
                                             std::array<ImageDimensions, 4>& dims,
                                             std::array<bool, 4>& dimsOk, std::array<bool, 4>& probed,
                                             const int16_t available) {
  if (available <= 0 || screen.body().width <= 0) return 0;
  const int16_t gap = screen.theme().spaceXs;
  int16_t left = available;
  int16_t consumed = 0;
  for (size_t i = 0; i < images.size(); ++i) {
    const auto& filename = images[i];
    if (filename.empty() || left < 24) break;
    const std::string path = mediaDirectory_ + "/" + filename;
    if (!probed[i]) {
      probed[i] = true;
      dimsOk[i] = PngToFramebufferConverter::getDimensionsStatic(path, dims[i]) && dims[i].width > 0 &&
                  dims[i].height > 0;
    }
    if (!dimsOk[i]) continue;
    const ImageDimensions& dimensions = dims[i];
    const int widthLimit = screen.body().width;
    size_t remaining = 0;
    for (size_t j = i; j < images.size() && !images[j].empty(); ++j) ++remaining;
    const int heightLimit = std::max<int>(1, left / static_cast<int>(remaining) - gap);
    const int width = std::min<int>(dimensions.width,
                                    std::min<int>(widthLimit, static_cast<int64_t>(dimensions.width) * heightLimit /
                                                                  dimensions.height));
    const int height = std::min<int>(dimensions.height,
                                     static_cast<int64_t>(dimensions.height) * width / dimensions.width);
    if (width <= 0 || height <= 0) continue;
    // Decode directly into the renderer's framebuffer. The UI layout only advances
    // after a successful decode; malformed/missing images leave text available.
    const auto area = screen.body();
    const int x = area.x + (area.width - width) / 2;
    RenderConfig config{x, area.y, width, height};
    config.useExactDimensions = true;
    PngToFramebufferConverter decoder;
    if (!decoder.decodeToFramebuffer(path, renderer, config)) continue;
    const int16_t used = std::min<int16_t>(area.height, static_cast<int16_t>(height + gap));
    screen.takeTop(used);
    consumed += used;
    left -= used;
  }
  return consumed;
}

bool StudySessionActivity::saveCheckpoint() {
  if (!opened_) return false;
  const bool saved = Study::saveCheckpoint(packagePath_, checkpoint_);
  errorMessage_ = saved ? nullptr : tr(STR_STUDY_SAVE_ERROR);
  return saved;
}

void StudySessionActivity::revealAnswer() {
  if (view_ != View::Front) return;
  checkpoint_.face = Study::CardFace::Back;
  view_ = View::Back;
  textTopLine_.store(0);
  saveCheckpoint();
  requestUpdate();
}

void StudySessionActivity::grade(const Study::Rating rating) {
  if (view_ != View::Back || checkpoint_.currentIndex == NO_CARD) return;
  Study::CardState updated = cardState_;
  const bool retryPass = checkpoint_.phase == Study::SessionPhase::Again;
  if (!Study::applyRating(updated, rating, today_, retryPass)) return;
  Study::Error error;
  if (!persistent_.save(checkpoint_.currentIndex, updated, error)) {
    errorMessage_ = tr(STR_STUDY_SAVE_ERROR);
    requestUpdate();
    return;
  }
  errorMessage_ = nullptr;
  cardState_ = updated;
  checkpoint_.currentIndex = NO_CARD;
  checkpoint_.currentCardId = 0;
  checkpoint_.face = Study::CardFace::Front;
  saveCheckpoint();
  findAndLoadNext();
  requestUpdate();
}

void StudySessionActivity::drawActions(UiScreen& screen, const int count) {
  if (view_ == View::Back) {
    drawRatingTiles(screen);
    return;
  }
  if (view_ == View::Front) {
    actionItems_[0].label = tr(STR_STUDY_SHOW_ANSWER);
    actionItems_[0].actionValue = 0;
  } else if (view_ == View::Done) {
    actionItems_[0].label = tr(STR_STUDY_AGAIN_SESSION);
    actionItems_[0].actionValue = 0;
    actionItems_[1].label = tr(STR_BACK);
    actionItems_[1].actionValue = 1;
  } else {
    actionItems_[0].label = tr(STR_BACK);
    actionItems_[0].actionValue = 0;
  }

  fui::ListProps props;
  props.items = actionItems_.data();
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.scrollIndicator = false;
  const int16_t band = actionBandHeight(screen, count);
  if (!mappedInput.hasTouch()) {
    props.rowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
  }
  syncListViewport(screen, props);
  screen.list(props, band, fui::LayoutAnchor::Bottom);
}

void StudySessionActivity::drawRatingTiles(UiScreen& screen) {
  const auto& theme = screen.theme();
  const int16_t gap = theme.spaceSm;
  const int16_t tileHeight = std::max<int16_t>(64, theme.rowHeight);
  const fui::Rect band = screen.takeBottom(actionBandHeight(screen, 4));
  const int16_t headingHeight = screen.target().lineHeight(theme.smallText.font);
  const int16_t tileWidth = static_cast<int16_t>((band.width - gap) / 2);
  if (tileWidth <= 0) return;

  auto heading = theme.smallText;
  heading.bold = true;
  screen.target().text(fui::Rect{band.x, band.y, band.width, headingHeight}, tr(STR_STUDY_RATE_PROMPT), heading);

  const char* labels[] = {tr(STR_STUDY_AGAIN), tr(STR_STUDY_HARD), tr(STR_STUDY_GOOD), tr(STR_STUDY_EASY)};
  const int16_t detailHeight = screen.target().lineHeight(theme.smallText.font);
  for (int i = 0; i < 4; ++i) {
    if (!ratingValid_[static_cast<size_t>(i)]) continue;
    const auto& interval = ratingIntervals_[static_cast<size_t>(i)];

    const bool selected = activeNav().selected == i;
    const fui::Rect tile{static_cast<int16_t>(band.x + (i % 2) * (tileWidth + gap)),
                         static_cast<int16_t>(band.y + headingHeight + gap + (i / 2) * (tileHeight + gap)),
                         tileWidth, tileHeight};
    fui::ButtonProps button;
    button.label = labels[i];
    button.action = ACTION_ROW;
    button.value = static_cast<int16_t>(i);
    button.inputMask = fui::InputTouch;
    button.state = selected ? fui::StateSelected : fui::StateNormal;
    button.text = theme.bodyText;
    button.styles = theme.button;
    button.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    button.styles.normal.borderWidth = 1;
    button.styles.selected.background = fui::Paint::solid(fui::Color::White);
    button.styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    button.styles.selected.border = fui::Paint::solid(fui::Color::Black);
    button.styles.selected.borderWidth = 3;
    button.radius = 6;
    button.padding = fui::Insets{2, 4, static_cast<int16_t>(detailHeight + 4), 4};
    screen.button(button, tile);

    auto detail = theme.smallText;
    detail.align = fui::TextAlign::Center;
    detail.color = fui::Color::Black;
    detail.inverted = false;
    screen.target().text(fui::Rect{tile.x, static_cast<int16_t>(tile.bottom() - detailHeight - 4),
                                   tile.width, detailHeight}, interval.data(), detail);
  }
}

void StudySessionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});

  if (view_ == View::Error || view_ == View::Empty) {
    auto message = screen.theme().bodyText;
    message.align = fui::TextAlign::Center;
    message.maxLines = 5;
    const char* text = view_ == View::Empty ? tr(STR_STUDY_DECK_EMPTY)
                                            : (errorMessage_ ? errorMessage_ : tr(STR_STUDY_OPEN_ERROR));
    const int16_t height = fui::measureWrappedText(screen.target(), text, message, screen.body().width).height;
    const int16_t usableHeight = std::max<int16_t>(
        0, static_cast<int16_t>(screen.body().height - actionBandHeight(screen, 1)));
    if (usableHeight > height) screen.spacer(static_cast<int16_t>((usableHeight - height) / 3));
    screen.target().text(screen.takeTop(std::min<int16_t>(height, usableHeight), screen.theme().spaceLg), text,
                         message);
    drawActions(screen, 1);
    return;
  }
  if (view_ == View::Done) {
    auto title = screen.theme().bodyText;
    title.align = fui::TextAlign::Center;
    title.bold = true;
    const int16_t usableHeight = std::max<int16_t>(
        0, static_cast<int16_t>(screen.body().height - actionBandHeight(screen, 2)));
    const char* text = tr(STR_STUDY_DONE);
    const int16_t titleHeight = screen.target().lineHeight(title.font);
    screen.target().text(screen.takeTop(std::min<int16_t>(titleHeight, usableHeight), screen.theme().spaceMd), text,
                         title);
    const Study::DeckInfo* deck = package_.deck(deckId_);
    if (deck && screen.body().height > 0) {
      auto deckStyle = screen.theme().bodyText;
      deckStyle.align = fui::TextAlign::Center;
      deckStyle.maxLines = 2;
      const int16_t deckHeight = fui::measureWrappedText(screen.target(), deck->name.c_str(), deckStyle,
                                                         screen.body().width).height;
      screen.target().text(screen.takeTop(std::min<int16_t>(deckHeight, screen.body().height),
                                          screen.theme().spaceMd),
                           deck->name.c_str(), deckStyle);
    }
    drawActions(screen, 2);
    return;
  }

  const Study::DeckInfo* deck = package_.deck(deckId_);
  auto context = screen.theme().smallText;
  context.bold = true;
  context.maxLines = 2;
  const char* deckName = deck ? deck->name.c_str() : tr(STR_STUDY);
  const int16_t overlineHeight = screen.target().lineHeight(context.font);
  auto overline = screen.theme().smallText;
  screen.target().text(screen.takeTop(std::min<int16_t>(overlineHeight, screen.body().height)),
                       tr(STR_STUDY_DECK_LABEL), overline);
  const int16_t headerHeight = fui::measureWrappedText(screen.target(), deckName, context, screen.body().width).height;
  screen.target().text(screen.takeTop(std::min<int16_t>(headerHeight, screen.body().height), screen.theme().spaceXs),
                       deckName, context);
  if (errorMessage_) {
    auto status = screen.theme().smallText;
    status.bold = true;
    screen.target().text(screen.takeTop(screen.target().lineHeight(status.font), screen.theme().spaceXs), errorMessage_,
                         status);
  }

  if (screen.body().height > 0) {
    const int16_t faceHeight = screen.target().lineHeight(context.font);
    const fui::Rect faceBand = screen.takeTop(std::min<int16_t>(faceHeight + screen.theme().spaceSm,
                                                              screen.body().height), screen.theme().spaceSm);
    auto faceStyle = screen.theme().smallText;
    faceStyle.bold = true;
    screen.target().text(fui::Rect{faceBand.x, faceBand.y, faceBand.width, faceHeight},
                         view_ == View::Front ? tr(STR_STUDY_QUESTION_LABEL) : tr(STR_STUDY_ANSWER_LABEL), faceStyle);
    screen.target().fill(fui::Rect{faceBand.x, static_cast<int16_t>(faceBand.bottom() - 1), faceBand.width, 1},
                         fui::Paint::solid(fui::Color::Black));
  }

  const fui::Rect body = screen.body();
  const std::string& frontText = frontNeedsDisplayNormalization_ ? frontDisplayText_ : card_.front;
  const std::string& backText = backNeedsDisplayNormalization_ ? backDisplayText_ : card_.back;
  if (view_ == View::Front) {
    const int16_t actionBand = actionBandHeight(screen, 1);
    const int16_t textHeight = std::max<int16_t>(
        0, static_cast<int16_t>(body.height - actionBand - screen.theme().spaceSm));
    const int16_t imageBudget = frontText.empty() ? textHeight : static_cast<int16_t>(textHeight / 2);
    const int16_t imageHeight =
        drawCardImages(screen, frontImages_, frontDims_, frontDimsOk_, frontDimsProbed_, imageBudget);
    drawCardText(screen, frontText, static_cast<int16_t>(textHeight - imageHeight), 0);
    drawActions(screen, 1);
    return;
  }

  const int16_t actionBand = actionBandHeight(screen, 4);
  const int16_t bodyHeightBeforeContext = screen.body().height;
  int16_t contextBand = 0;
  if (!backStartsWithFront_ && !frontText.empty()) {
    auto frontStyle = screen.theme().smallText;
    frontStyle.maxLines = 2;
    frontStyle.bold = true;
    const int16_t lineHeight = screen.target().lineHeight(frontStyle.font);
    const int16_t maxContextHeight = static_cast<int16_t>(lineHeight * 2);
    const int16_t contextHeight = std::min<int16_t>(
        maxContextHeight, fui::measureWrappedText(screen.target(), frontText.c_str(), frontStyle,
                                                  screen.body().width).height);
    const int16_t contextGap = screen.theme().spaceXs;
    screen.target().text(screen.takeTop(contextHeight, contextGap), frontText.c_str(), frontStyle);
    contextBand = static_cast<int16_t>(contextHeight + contextGap);
  }
  const int16_t remainingBody = std::min<int16_t>(
      screen.body().height,
      static_cast<int16_t>(std::max<int16_t>(0, bodyHeightBeforeContext - contextBand)));
  const int16_t textHeight = std::max<int16_t>(
      0, static_cast<int16_t>(remainingBody - actionBand - screen.theme().spaceSm));
  const int16_t imageHeight =
      drawCardImages(screen, backImages_, backDims_, backDimsOk_, backDimsProbed_,
                     backText.empty() ? textHeight : static_cast<int16_t>(textHeight / 2));
  drawCardText(screen, backText, static_cast<int16_t>(textHeight - imageHeight), 1);
  drawActions(screen, 4);
}

void StudySessionActivity::activateIndex(const int index) {
  RenderLock lock(*this);
  if (view_ == View::Front) {
    revealAnswer();
  } else if (view_ == View::Back && index >= 0 && index < 4) {
    grade(static_cast<Study::Rating>(index + 1));
  } else if (view_ == View::Done && index == 0) {
    // Done means nothing is due, so a plain Main restart would land straight
    // back on Done. Cram revisits every card once; Again answers still retry.
    Study::clearCheckpoint(packagePath_);
    checkpoint_.phase = Study::SessionPhase::Cram;
    checkpoint_.nextIndex = 0;
    checkpoint_.currentIndex = NO_CARD;
    checkpoint_.currentCardId = 0;
    findAndLoadNext();
    requestUpdate();
  } else if (view_ == View::Done || view_ == View::Empty || view_ == View::Error) {
    finish();
  }
}
