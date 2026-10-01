#include "HighlightStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>

namespace HighlightStore {
namespace {

constexpr char DIRECTORY[] = "/.crosspoint/highlights";
constexpr std::array<uint8_t, 4> MAGIC = {'H', 'L', 'T', '1'};
constexpr uint32_t CRC_INITIAL = 0xFFFFFFFFu;

uint32_t crc32Update(uint32_t crc, const uint8_t* bytes, const size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

bool writeBytes(HalFile& file, const void* data, const size_t size, uint32_t& crc) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (size > 0 && file.write(bytes, size) != size) return false;
  crc = crc32Update(crc, bytes, size);
  return true;
}

bool readBytes(HalFile& file, void* data, const size_t size, uint32_t& crc) {
  auto* bytes = static_cast<uint8_t*>(data);
  if (size > 0 && file.read(bytes, size) != static_cast<int>(size)) return false;
  crc = crc32Update(crc, bytes, size);
  return true;
}

template <typename T>
bool writePod(HalFile& file, const T value, uint32_t& crc) {
  static_assert(std::is_unsigned_v<T>);
  std::array<uint8_t, sizeof(T)> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
  return writeBytes(file, bytes.data(), bytes.size(), crc);
}

template <typename T>
bool readPod(HalFile& file, T& value, uint32_t& crc) {
  static_assert(std::is_unsigned_v<T>);
  std::array<uint8_t, sizeof(T)> bytes{};
  if (!readBytes(file, bytes.data(), bytes.size(), crc)) return false;
  value = 0;
  for (size_t i = 0; i < bytes.size(); ++i) value |= static_cast<T>(bytes[i]) << (i * 8);
  return true;
}

bool writeString(HalFile& file, const std::string& value, const uint16_t maxBytes, uint32_t& crc) {
  if (value.size() > maxBytes || !writePod(file, static_cast<uint16_t>(value.size()), crc)) return false;
  return writeBytes(file, value.data(), value.size(), crc);
}

bool readString(HalFile& file, std::string& value, const uint16_t maxBytes, uint32_t& crc) {
  uint16_t size = 0;
  if (!readPod(file, size, crc) || size > maxBytes) return false;
  value.resize(size);
  return readBytes(file, value.data(), size, crc);
}

bool readFile(const std::string& path, const std::string& expectedBookPath, std::vector<HighlightEntry>& out) {
  HalFile file;
  if (!Storage.openFileForRead("HLT", path, file)) return false;

  uint32_t crc = CRC_INITIAL;
  std::array<uint8_t, MAGIC.size()> magic{};
  uint8_t version = 0;
  uint16_t count = 0;
  std::string storedPath;
  bool valid = readBytes(file, magic.data(), magic.size(), crc) && magic == MAGIC && readPod(file, version, crc) &&
               (version == FORMAT_VERSION || version == LEGACY_FORMAT_VERSION) && readPod(file, count, crc) &&
               count <= MAX_HIGHLIGHTS_PER_BOOK &&
               readString(file, storedPath, MAX_BOOK_PATH_BYTES, crc) && storedPath == expectedBookPath;

  std::vector<HighlightEntry> loaded;
  if (valid) loaded.reserve(count);
  for (uint16_t i = 0; valid && i < count; ++i) {
    HighlightEntry entry;
    valid = readPod(file, entry.id, crc) && readPod(file, entry.spineIndex, crc) &&
            readPod(file, entry.startOffset, crc) && readPod(file, entry.endOffset, crc) &&
            readString(file, entry.quote, HighlightEntry::MAX_QUOTE_BYTES, crc);
    // V1 records ended at quote; an absent note migrates to the model's empty
    // default. Every subsequent save writes the v2 note-length field.
    if (valid && version >= FORMAT_VERSION) {
      valid = readString(file, entry.note, HighlightEntry::MAX_NOTE_BYTES, crc);
    }
    if (valid) valid = entry.isValid();
    if (valid) loaded.push_back(std::move(entry));
  }

  uint32_t storedCrc = 0;
  if (valid) {
    // The checksum trailer itself is not included in the checksum.
    std::array<uint8_t, sizeof(storedCrc)> bytes{};
    valid = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
    if (valid) {
      for (size_t i = 0; i < bytes.size(); ++i) storedCrc |= static_cast<uint32_t>(bytes[i]) << (i * 8);
      valid = storedCrc == ~crc && file.available() == 0;
    }
  }
  file.close();
  if (!valid) return false;
  out = std::move(loaded);
  return true;
}

bool writeFile(const std::string& path, const std::string& bookPath, const std::vector<HighlightEntry>& entries) {
  HalFile file;
  if (!Storage.openFileForWrite("HLT", path, file)) return false;
  uint32_t crc = CRC_INITIAL;
  bool ok = writeBytes(file, MAGIC.data(), MAGIC.size(), crc) && writePod(file, FORMAT_VERSION, crc) &&
            writePod(file, static_cast<uint16_t>(entries.size()), crc) &&
            writeString(file, bookPath, MAX_BOOK_PATH_BYTES, crc);
  for (const auto& entry : entries) {
    if (!ok) break;
    ok = writePod(file, entry.id, crc) && writePod(file, entry.spineIndex, crc) &&
         writePod(file, entry.startOffset, crc) && writePod(file, entry.endOffset, crc) &&
         writeString(file, entry.quote, HighlightEntry::MAX_QUOTE_BYTES, crc) &&
         writeString(file, entry.note, HighlightEntry::MAX_NOTE_BYTES, crc);
  }
  const uint32_t finalCrc = ~crc;
  if (ok) {
    std::array<uint8_t, sizeof(finalCrc)> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(finalCrc >> (i * 8));
    ok = file.write(bytes.data(), bytes.size()) == bytes.size();
  }
  if (ok) file.flush();
  const bool closeOk = file.close();
  return ok && closeOk;
}

bool recoverBackup(const std::string& canonical, const std::string& backup) {
  if (!Storage.exists(backup.c_str())) return false;
  if (Storage.exists(canonical.c_str()) && !Storage.remove(canonical.c_str())) return false;
  return Storage.rename(backup.c_str(), canonical.c_str());
}

}  // namespace

std::string getFilePath(const std::string& bookPath) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : bookPath) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  char suffix[17];
  snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(hash));
  return std::string(DIRECTORY) + "/highlight_" + suffix + ".bin";
}

bool load(const std::string& bookPath, std::vector<HighlightEntry>& entries) {
  if (bookPath.empty() || bookPath.size() > MAX_BOOK_PATH_BYTES) return false;
  const std::string canonical = getFilePath(bookPath);
  const std::string backup = canonical + ".bak";
  if (!Storage.exists(canonical.c_str()) && !Storage.exists(backup.c_str())) {
    entries.clear();
    return true;
  }

  std::vector<HighlightEntry> loaded;
  if (Storage.exists(canonical.c_str()) && readFile(canonical, bookPath, loaded)) {
    entries = std::move(loaded);
    return true;
  }

  // A validated backup is the only recovery path for a damaged/missing
  // canonical. Do not replace caller state with partially decoded records.
  if (!Storage.exists(backup.c_str()) || !readFile(backup, bookPath, loaded)) return false;
  if (!recoverBackup(canonical, backup)) return false;
  entries = std::move(loaded);
  return true;
}

bool save(const std::string& bookPath, const std::vector<HighlightEntry>& entries) {
  if (bookPath.empty() || bookPath.size() > MAX_BOOK_PATH_BYTES || entries.size() > MAX_HIGHLIGHTS_PER_BOOK) {
    return false;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].isValid()) return false;
    for (size_t j = 0; j < i; ++j) {
      if (entries[i].id == entries[j].id) return false;
    }
  }

  if (!Storage.mkdir("/.crosspoint") && !Storage.exists("/.crosspoint")) return false;
  if (!Storage.mkdir(DIRECTORY) && !Storage.exists(DIRECTORY)) return false;

  const std::string canonical = getFilePath(bookPath);
  const std::string temporary = canonical + ".tmp";
  const std::string backup = canonical + ".bak";
  if (Storage.exists(temporary.c_str()) && !Storage.remove(temporary.c_str())) return false;

  // Never discard the only known-good copy while trying to replace a damaged
  // canonical. Validate/recover first; a caller may save only after a
  // successful load, but the storage boundary must enforce this invariant too.
  if (Storage.exists(canonical.c_str())) {
    std::vector<HighlightEntry> existing;
    if (!readFile(canonical, bookPath, existing)) {
      std::vector<HighlightEntry> previous;
      if (!Storage.exists(backup.c_str()) || !readFile(backup, bookPath, previous) ||
          !recoverBackup(canonical, backup)) {
        return false;
      }
    }
  } else if (Storage.exists(backup.c_str()) && !recoverBackup(canonical, backup)) {
    return false;
  }
  if (!writeFile(temporary, bookPath, entries)) {
    Storage.remove(temporary.c_str());
    return false;
  }

  const bool hadCanonical = Storage.exists(canonical.c_str());
  if (hadCanonical) {
    // Any pre-existing backup is stale now: canonical was validated above.
    if (Storage.exists(backup.c_str()) && !Storage.remove(backup.c_str())) {
      Storage.remove(temporary.c_str());
      return false;
    }
    if (!Storage.rename(canonical.c_str(), backup.c_str())) {
      Storage.remove(temporary.c_str());
      return false;
    }
  }
  if (!Storage.rename(temporary.c_str(), canonical.c_str())) {
    if (hadCanonical) Storage.rename(backup.c_str(), canonical.c_str());
    Storage.remove(temporary.c_str());
    return false;
  }
  if (hadCanonical) Storage.remove(backup.c_str());
  return true;
}

bool listBookPaths(std::vector<std::string>& paths) {
  paths.clear();
  if (!Storage.exists(DIRECTORY)) return true;
  HalFile directory;
  if (!Storage.openFileForRead("HLT", DIRECTORY, directory) || !directory.isDirectory()) {
    directory.close();
    return false;
  }
  bool ok = true;
  std::vector<std::string> candidates;
  for (auto file = directory.openNextFile(); file; file = directory.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    char name[128]{};
    file.getName(name, sizeof(name));
    const std::string filename(name);
    const bool backup = filename.size() >= 18 && filename.compare(filename.size() - 8, 8, ".bin.bak") == 0;
    if (filename.size() < 14 || filename.compare(0, 10, "highlight_") != 0 ||
        (!backup && filename.compare(filename.size() - 4, 4, ".bin") != 0)) {
      file.close();
      continue;
    }
    uint32_t crc = CRC_INITIAL;
    std::array<uint8_t, MAGIC.size()> magic{};
    uint8_t version = 0;
    uint16_t count = 0;
    std::string path;
    const bool headerOk = readBytes(file, magic.data(), magic.size(), crc) && magic == MAGIC &&
                          readPod(file, version, crc) &&
                          (version == FORMAT_VERSION || version == LEGACY_FORMAT_VERSION) &&
                          readPod(file, count, crc) && count <= MAX_HIGHLIGHTS_PER_BOOK &&
                          readString(file, path, MAX_BOOK_PATH_BYTES, crc);
    file.close();
    if (!headerOk || path.empty() ||
        getFilePath(path) + (backup ? ".bak" : "") != std::string(DIRECTORY) + "/" + filename) {
      ok = false;
      continue;
    }
    if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) candidates.push_back(std::move(path));
  }
  directory.close();  // load() may recover/rename a file; do not keep the directory iterator open.
  for (const auto& path : candidates) {
    std::vector<HighlightEntry> entries;
    if (!load(path, entries)) {
      ok = false;
      continue;
    }
    if (!entries.empty()) paths.push_back(path);
  }
  std::sort(paths.begin(), paths.end());
  return ok;
}

}  // namespace HighlightStore
