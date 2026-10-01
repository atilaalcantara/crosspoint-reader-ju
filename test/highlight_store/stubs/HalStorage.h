#pragma once

#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <string>

class HalFile {
 public:
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&&) noexcept = default;
  HalFile& operator=(HalFile&&) noexcept = default;

  int read(void* data, const size_t size) {
    stream_.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<int>(stream_.gcount());
  }
  size_t write(const uint8_t* data, const size_t size) {
    stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return stream_ ? size : 0;
  }
  void flush() { stream_.flush(); }
  bool close() {
    if (isDirectory()) {
      directory_.clear();
      iterator_ = {};
      return true;
    }
    const bool ok = stream_.good();
    stream_.close();
    return ok && !stream_.fail();
  }
  int available() {
    const int next = stream_.peek();
    if (next == std::char_traits<char>::eof()) {
      stream_.clear(stream_.rdstate() & ~std::ios::eofbit);
      return 0;
    }
    return 1;
  }
  bool openRead(const std::filesystem::path& path) {
    if (std::filesystem::is_directory(path)) {
      directory_ = path;
      iterator_ = std::filesystem::directory_iterator(path);
      return true;
    }
    stream_.open(path, std::ios::binary | std::ios::in);
    return stream_.is_open();
  }
  bool openWrite(const std::filesystem::path& path) {
    stream_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    return stream_.is_open();
  }
  bool isDirectory() const { return !directory_.empty(); }
  explicit operator bool() const { return stream_.is_open() || isDirectory(); }
  size_t getName(char* dest, size_t len) const {
    const auto name = name_.filename().string();
    if (len) {
      const auto size = std::min(name.size(), len - 1);
      std::memcpy(dest, name.data(), size);
      dest[size] = 0;
    }
    return name.size();
  }
  HalFile openNextFile() {
    HalFile file;
    if (!isDirectory() || iterator_ == std::filesystem::directory_iterator{}) return file;
    const auto path = iterator_->path();
    ++iterator_;
    file.name_ = path;
    file.openRead(path);
    return file;
  }

 private:
  std::fstream stream_;
  std::filesystem::path directory_;
  std::filesystem::path name_;
  std::filesystem::directory_iterator iterator_;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.openRead(map(path)); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.openWrite(map(path)); }
  bool exists(const char* path) const { return std::filesystem::exists(map(path)); }
  bool mkdir(const char* path, bool = true) {
    std::error_code error;
    std::filesystem::create_directories(map(path), error);
    return !error;
  }
  bool remove(const char* path) {
    std::error_code error;
    const bool removed = std::filesystem::remove(map(path), error);
    return removed && !error;
  }
  bool rename(const char* from, const char* to) {
    const auto destination = map(to);
    if (std::filesystem::exists(destination)) return false;
    std::error_code error;
    std::filesystem::rename(map(from), destination, error);
    return !error;
  }

 private:
  static std::filesystem::path map(const std::string& path) {
    return std::filesystem::temp_directory_path() / "crosspoint-highlight-store-test" /
           std::filesystem::path(path).relative_path();
  }
};

#define Storage HalStorage::getInstance()
