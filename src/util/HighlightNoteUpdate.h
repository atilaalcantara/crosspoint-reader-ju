#pragma once

#include <string>
#include <utility>

#include "../HighlightEntry.h"

namespace HighlightNoteUpdate {

enum class Result { Cancelled, Invalid, Saved, SaveFailed };

template <typename Save>
Result apply(HighlightEntry& highlight, const bool cancelled, std::string replacement, Save&& save) {
  if (cancelled) return Result::Cancelled;
  if (replacement.size() > HighlightEntry::MAX_NOTE_BYTES || !HighlightEntry::isValidNoteUtf8(replacement)) {
    return Result::Invalid;
  }

  std::string previous = std::move(highlight.note);
  highlight.note = std::move(replacement);
  if (save()) return Result::Saved;
  highlight.note = std::move(previous);
  return Result::SaveFailed;
}

}  // namespace HighlightNoteUpdate
