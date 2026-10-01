#include "StudyFixtureBuilder.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {
bool write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return output.good();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  const std::filesystem::path populated(argv[1]);
  const std::filesystem::path empty(argv[2]);
  const std::filesystem::path invalid(argv[3]);
  const std::filesystem::path portuguese(argv[4]);
  std::filesystem::remove_all(populated / "study");
  std::filesystem::remove_all(empty / "study");
  std::filesystem::remove_all(invalid / "study");
  std::filesystem::remove_all(portuguese / "study");
  std::filesystem::remove_all(portuguese / ".crosspoint");
  const auto package = makeStudyFixture(
      {{1, 0, "Preparatório"}, {2, 1, "Virologia"}, {3, 1, "Clínica"}},
      {{101, 2, "O que significa HIV?", "Vírus da imunodeficiência humana.\nA resposta preserva Unicode: café e coração."},
       {102, 2, "Qual é o agente da raiva?", "Lyssavirus."},
       {103, 3, "Sinal clínico\nmultilinha?", "Resposta com acentuação: coração."},
       {104, 3, "Como se chama febre?", "Elevação da temperatura corporal."}});
  if (!write(populated / "study" / "demo.cps", package)) return 1;
  if (!write(populated / "study" / "broken.cps", {'n', 'o', 't', ' ', 'a', ' ', 'p', 'a', 'c', 'k', 'a', 'g', 'e'}))
    return 1;
  std::filesystem::create_directories(empty / "study");
  if (!write(invalid / "study" / "broken.cps", {'n', 'o', 't', ' ', 'a', ' ', 'p', 'a', 'c', 'k', 'a', 'g', 'e'}))
    return 1;
  if (!write(portuguese / "study" / "demo.cps", package)) return 1;
  std::filesystem::create_directories(portuguese / ".crosspoint");
  std::ofstream settings(portuguese / ".crosspoint" / "settings.json", std::ios::trunc);
  settings << "{\"language\":\"PT\"}";
  if (!settings.good()) return 1;
  return 0;
}
