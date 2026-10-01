#pragma once
#include <stdint.h>

#include <random>

inline uint32_t esp_random() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<uint32_t> dist(0, UINT32_MAX);
  return dist(gen);
}
