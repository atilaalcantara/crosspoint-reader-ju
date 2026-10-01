#pragma once

#include <HalStorage.h>

#include "StudyPackage.h"

namespace Study {

class HalFileAccess final : public WritableRandomAccess {
 public:
  explicit HalFileAccess(HalFile& file) : file_(file) {}
  uint64_t size() const override;
  bool readAt(uint32_t offset, void* destination, size_t length) override;
  bool writeAt(uint32_t offset, const void* source, size_t length) override;
  bool flush() override;

 private:
  HalFile& file_;
};

// Scans only direct files under /study/. A package that fails validation is
// retained as a per-file error while other valid packages remain available.
// Idempotently creates /study before first use; fails if storage is unavailable
// or the path exists as a non-directory.
bool ensureStudyDirectory();
bool discoverFromSd(Catalog& catalog);

}  // namespace Study
