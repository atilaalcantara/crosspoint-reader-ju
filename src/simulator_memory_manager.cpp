#if defined(SIMULATOR)

#include "MemoryManager.h"

namespace freeink {

MemoryManager &MemoryManager::instance() {
  static MemoryManager manager;
  return manager;
}

int MemoryManager::registerSink(const CacheSink &) { return 1; }

size_t MemoryManager::freeBytes(MemPool) const { return SIZE_MAX; }

bool MemoryManager::ensureFree(size_t, MemPool) { return true; }

} // namespace freeink

#endif
