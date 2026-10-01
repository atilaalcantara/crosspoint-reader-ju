#include "StudyStorage.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace Study {
namespace {

constexpr char STUDY_DIRECTORY[] = "/study";
constexpr int MAX_DISCOVERED_FILES = 512;

bool hasPackageSuffix(const String& name) {
  const size_t length = name.length();
  constexpr char suffix[] = ".cps";
  if (length < sizeof(suffix) - 1) return false;
  for (size_t i = 0; i < sizeof(suffix) - 1; ++i) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(name[length - sizeof(suffix) + 1 + i]))) != suffix[i])
      return false;
  }
  return true;
}

}  // namespace

uint64_t HalFileAccess::size() const { return file_.fileSize64(); }

bool HalFileAccess::readAt(uint32_t offset, void* destination, size_t length) {
  if (static_cast<uint64_t>(offset) + length > file_.fileSize64() || !file_.seek64(offset)) return false;
  return file_.read(destination, length) == static_cast<int>(length);
}

bool HalFileAccess::writeAt(uint32_t offset, const void* source, size_t length) {
  if (!file_.seek64(offset)) return false;
  return file_.write(source, length) == length;
}

bool HalFileAccess::flush() {
  file_.flush();
  return true;
}

bool ensureStudyDirectory() {
  if (!Storage.ensureDirectoryExists(STUDY_DIRECTORY)) return false;
  HalFile directory = Storage.open(STUDY_DIRECTORY);
  if (!directory) return false;
  const bool isDirectory = directory.isDirectory();
  directory.close();
  return isDirectory;
}

bool discoverFromSd(Catalog& catalog) {
  catalog.clear();
  const auto files = Storage.listFiles(STUDY_DIRECTORY, MAX_DISCOVERED_FILES);
  for (const auto& name : files) {
    if (!hasPackageSuffix(name)) continue;
    const std::string path = std::string(STUDY_DIRECTORY) + "/" + name.c_str();
    HalFile file;
    if (!Storage.openFileForRead("STUDY", path, file)) {
      // Keep the I/O failure visible as an invalid candidate.
      catalog.addError(path, Error::Io);
      continue;
    }
    HalFileAccess access(file);
    catalog.addFile(path, access);
    catalog.releaseCardIndexes();
    file.close();
  }
  if (files.size() >= MAX_DISCOVERED_FILES) {
    catalog.addError(STUDY_DIRECTORY, Error::LimitExceeded);
    return false;
  }
  return true;
}

}  // namespace Study
