#pragma once

#include <array>
#include <atomic>
#include <string>

#include "activities/UiListActivity.h"
#include "study/StudyPackage.h"
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include "study/StudyMedia.h"
#include "study/StudyPersistence.h"
#include "study/StudyStorage.h"

class StudySessionActivity final : public UiListActivity {
 public:
  StudySessionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string packagePath,
                       uint32_t packageCrc, uint32_t deckId);
  void onEnter() override;
  void onExit() override;

 protected:
  void onBackButton() override;
  bool handleCustomInput() override;

 private:
  enum class View : uint8_t { Error, Front, Back, Done, Empty };

  std::string packagePath_;
  uint32_t expectedPackageCrc_ = 0;
  uint32_t deckId_ = 0;
  HalFile packageFile_;
  Study::HalFileAccess packageAccess_;
  Study::Package package_;
  HalFile mediaFile_;
  Study::HalFileAccess mediaAccess_{mediaFile_};
  Study::MediaIndex mediaIndex_;
  Study::MediaIndex::Names frontImages_{};
  Study::MediaIndex::Names backImages_{};
  // PNG header probes per card face, filled lazily on first draw: the header
  // parse otherwise repeats on every scroll frame.
  std::array<ImageDimensions, 4> frontDims_{};
  std::array<ImageDimensions, 4> backDims_{};
  std::array<bool, 4> frontDimsOk_{};
  std::array<bool, 4> backDimsOk_{};
  std::array<bool, 4> frontDimsProbed_{};
  std::array<bool, 4> backDimsProbed_{};
  std::string mediaDirectory_;
  bool mediaChecked_ = false;
  Study::PersistentState persistent_;
  Study::Card card_;
  Study::CardState cardState_;
  std::string textLineBuffer_;
  std::string frontDisplayText_;
  std::string backDisplayText_;
  std::array<freeink::ui::ListItem, 4> actionItems_{};
  // Per-card preview of each rating's outcome, computed once in loadCurrent
  // so the render path does no scheduling work (matters on ESP32-S3).
  std::array<std::array<char, 24>, 4> ratingIntervals_{};
  std::array<bool, 4> ratingValid_{};
  // Layout cache: text line counts per card face at a given width. Scroll
  // frames otherwise pay two full text-measurement walks before drawing.
  struct TextLayoutCache {
    uint32_t generation = 0;
    int16_t viewportWidth = 0;
    int16_t textWidth = 0;
    uint32_t lines = 0;
    bool valid = false;
  };
  std::array<TextLayoutCache, 2> layoutCache_{};
  uint32_t layoutGeneration_ = 0;
  Study::SessionCheckpoint checkpoint_;
  View view_ = View::Error;
  const char* errorMessage_ = nullptr;
  std::atomic<uint32_t> textTopLine_{0};
  std::atomic<uint32_t> maxTextTopLine_{0};
  std::atomic<uint16_t> visibleTextLines_{0};
  bool frontNeedsDisplayNormalization_ = false;
  bool backNeedsDisplayNormalization_ = false;
  int32_t today_ = 0;
  bool opened_ = false;
  bool backStartsWithFront_ = false;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawActions(UiScreen& screen, int count);
  void drawRatingTiles(UiScreen& screen);
  int16_t actionBandHeight(UiScreen& screen, int count) const;
  uint32_t drawCardText(UiScreen& screen, const std::string& text, int16_t height, int cacheSlot);
  int16_t drawCardImages(UiScreen& screen, const Study::MediaIndex::Names& images,
                           std::array<ImageDimensions, 4>& dims, std::array<bool, 4>& dimsOk,
                           std::array<bool, 4>& probed, int16_t available);
  bool loadSession();
  bool findAndLoadNext();
  bool loadCurrent(uint32_t index, uint64_t expectedId, Study::CardFace face);
  void revealAnswer();
  void grade(Study::Rating rating);
  bool saveCheckpoint();
  static bool lookupState(void* context, uint32_t index, uint64_t cardId, Study::CardState& state,
                          Study::Error& error);
};
