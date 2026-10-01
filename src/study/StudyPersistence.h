#pragma once

#include <HalStorage.h>

#include <string>

#include "StudyScheduler.h"
#include "StudyStorage.h"

namespace Study {

std::string statePathForPackage(const std::string& packagePath);
std::string checkpointPathForPackage(const std::string& packagePath);

class PersistentState {
 public:
  PersistentState() : access_(file_) {}
  bool open(const std::string& packagePath, uint32_t packageCrc, uint32_t cardCount, Error& error);
  void close();
  bool load(uint32_t cardIndex, uint64_t cardId, CardState& state, Error& error);
  bool save(uint32_t cardIndex, const CardState& state, Error& error);

 private:
  HalFile file_;
  HalFileAccess access_;
  StateStore store_;
  bool opened_ = false;
};

bool loadCheckpoint(const std::string& packagePath, uint32_t packageCrc, uint32_t cardCount,
                    SessionCheckpoint& checkpoint);
bool saveCheckpoint(const std::string& packagePath, const SessionCheckpoint& checkpoint);
bool clearCheckpoint(const std::string& packagePath);

}  // namespace Study
