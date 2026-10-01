#pragma once

#include <stdint.h>

namespace freeink {

enum class XteinkVerdict : uint8_t { X4Confirmed, X3Confirmed, Inconclusive };

inline XteinkVerdict detectXteinkVerdict(uint8_t* score1 = nullptr,
                                         uint8_t* score2 = nullptr) {
  if (score1) *score1 = 0;
  if (score2) *score2 = 0;
  return XteinkVerdict::Inconclusive;
}

inline bool detectXteinkIsX3() { return false; }

enum class DisplayControllerVerdict : uint8_t {
  PrimaryAssumed,
  Uc81xxConfirmed,
  Inconclusive
};

struct XteinkDisplayProbeDiag {
  bool valid = false;
  uint8_t ver[5] = {0};
  uint8_t flg = 0;
  uint8_t verdict = 0;
  bool promoted = false;
  bool mtpValid = false;
  uint8_t mtp[48] = {0};
};

inline DisplayControllerVerdict detectXteinkDisplayController(
    uint8_t* = nullptr, uint8_t* = nullptr) {
  return DisplayControllerVerdict::Inconclusive;
}

inline const XteinkDisplayProbeDiag& getXteinkDisplayProbeDiag() {
  static const XteinkDisplayProbeDiag empty;
  return empty;
}

inline bool applyXteinkDisplayController() { return false; }

}  // namespace freeink
