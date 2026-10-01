#include <study/StudyPackage.h>

#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
class FileSource final : public Study::RandomAccess {
 public:
  explicit FileSource(const std::string& path) : path_(path), file_(path, std::ios::binary) {}
  bool good() const { return file_.good(); }
  uint64_t size() const override {
    std::error_code error;
    const auto size = std::filesystem::file_size(path_, error);
    return error ? 0 : size;
  }
  bool readAt(const uint32_t offset, void* destination, const size_t length) override {
    file_.clear();
    file_.seekg(offset);
    file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(length));
    return file_.gcount() == static_cast<std::streamsize>(length);
  }

 private:
  std::string path_;
  std::ifstream file_;
};
}  // namespace

int main(const int argc, char** argv) {
  const bool openOnly = argc == 3 && std::string(argv[2]) == "--open-only";
  if (argc != 2 && !openOnly) return 2;
  FileSource file(argv[1]);
  if (!file.good()) return 3;
  Study::Package package;
  Study::Error error;
  const auto openStart = std::chrono::steady_clock::now();
  const bool opened = package.open(file, error);
  const auto openElapsed = std::chrono::steady_clock::now() - openStart;
  if (!opened) {
    std::cerr << Study::errorName(error) << '\n';
    return 1;
  }
  std::cout << "OK " << package.deckCount() << ' ' << package.cardCount() << ' ' << package.packageCrc() << '\n';
  for (const auto& deck : package.decks())
    std::cout << "DECK " << deck.id << ' ' << deck.parentId << ' ' << deck.cardCount << ' ' << deck.subtreeCardCount
              << '\n';
  if (openOnly) {
    std::cout << "OPEN_US " << std::chrono::duration_cast<std::chrono::microseconds>(openElapsed).count() << '\n';
    return 0;
  }
  for (uint32_t i = 0; i < package.cardCount(); ++i) {
    Study::Card card;
    if (!package.loadCard(file, i, card, error)) return 1;
    std::cout << "CARD " << i << ' ' << card.id << ' ' << card.deckId << ' ' << card.front.size() << ' '
              << card.back.size() << '\n';
  }
  return 0;
}
