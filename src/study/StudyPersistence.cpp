#include "StudyPersistence.h"

#include <algorithm>
#include <array>

namespace Study {
namespace {

bool validPackagePath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash + 1 >= path.size()) return false;
  const std::string name = path.substr(slash + 1);
  if (name.size() <= 4) return false;
  const size_t ext = name.size() - 4;
  return name[ext] == '.' && (name[ext + 1] == 'c' || name[ext + 1] == 'C') &&
         (name[ext + 2] == 'p' || name[ext + 2] == 'P') && (name[ext + 3] == 's' || name[ext + 3] == 'S') &&
         name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

enum class CheckpointFileStatus : uint8_t { Missing, Valid, Invalid, IoError };

CheckpointFileStatus readCheckpointStatus(const std::string& path, const uint32_t packageCrc, const uint32_t cardCount,
                                          SessionCheckpoint& checkpoint) {
  HalFile file;
  if (!Storage.openFileForRead("STUDY", path, file))
    return Storage.exists(path.c_str()) ? CheckpointFileStatus::IoError : CheckpointFileStatus::Missing;
  if (file.fileSize64() != CHECKPOINT_BYTES) {
    file.close();
    return CheckpointFileStatus::Invalid;
  }
  std::array<uint8_t, CHECKPOINT_BYTES> bytes{};
  const bool read = file.read(bytes.data(), bytes.size()) == static_cast<int>(bytes.size());
  const bool close = file.close();
  if (!read || !close) return CheckpointFileStatus::IoError;
  return decodeCheckpoint(bytes.data(), bytes.size(), packageCrc, cardCount, checkpoint) ? CheckpointFileStatus::Valid
                                                                                       : CheckpointFileStatus::Invalid;
}

bool readCheckpointFile(const std::string& path, const uint32_t packageCrc, const uint32_t cardCount,
                        SessionCheckpoint& checkpoint) {
  return readCheckpointStatus(path, packageCrc, cardCount, checkpoint) == CheckpointFileStatus::Valid;
}

bool checkpointPaths(const std::string& packagePath, std::string& canonical, std::string& temporary,
                     std::string& backup) {
  canonical = checkpointPathForPackage(packagePath);
  if (canonical.empty()) return false;
  temporary = canonical + ".tmp";
  backup = canonical + ".bak";
  return true;
}

}  // namespace

std::string statePathForPackage(const std::string& packagePath) {
  if (!validPackagePath(packagePath)) return {};
  return packagePath.substr(0, packagePath.size() - 4) + ".state";
}

std::string checkpointPathForPackage(const std::string& packagePath) {
  if (!validPackagePath(packagePath)) return {};
  return packagePath.substr(0, packagePath.size() - 4) + ".session";
}

void PersistentState::close() {
  // HalFile::close() asserts on a never-opened file; only close a live
  // handle. This keeps open()/onExit() safe on fresh or failed sessions.
  if (file_.isOpen()) file_.close();
  opened_ = false;
}

bool PersistentState::open(const std::string& packagePath, const uint32_t packageCrc, const uint32_t cardCount,
                           Error& error) {
  close();
  error = Error::None;
  const std::string canonical = statePathForPackage(packagePath);
  if (canonical.empty() || cardCount > MAX_CARDS) {
    error = Error::InvalidHeader;
    return false;
  }
  const std::string backup = canonical + ".bak";
  const std::string temporary = canonical + ".tmp";

  const auto tryOpen = [&](const std::string& candidate, const bool promote) -> bool {
    HalFile file = Storage.open(candidate.c_str(), O_RDWR);
    if (!file) return false;
    HalFileAccess access(file);
    StateStore candidateStore;
    Error candidateError = Error::None;
    if (!candidateStore.open(access, packageCrc, cardCount, candidateError)) {
      file.close();
      error = candidateError;
      return false;
    }
    if (promote) {
      file.close();
      if (Storage.exists(canonical.c_str()) && !Storage.remove(canonical.c_str())) {
        error = Error::Io;
        return false;
      }
      if (!Storage.rename(candidate.c_str(), canonical.c_str())) {
        error = Error::Io;
        return false;
      }
      file = Storage.open(canonical.c_str(), O_RDWR);
      if (!file) {
        error = Error::Io;
        return false;
      }
      HalFileAccess promotedAccess(file);
      if (!candidateStore.open(promotedAccess, packageCrc, cardCount, candidateError)) {
        file.close();
        error = candidateError;
        return false;
      }
    }
    file_ = std::move(file);
    store_ = candidateStore;
    opened_ = true;
    error = Error::None;
    return true;
  };

  // The canonical is authoritative when valid; otherwise recover only from a
  // fully validated backup or temporary file.
  if (Storage.exists(canonical.c_str()) && tryOpen(canonical, false)) return true;
  if (Storage.exists(backup.c_str()) && tryOpen(backup, true)) return true;
  if (!Storage.exists(canonical.c_str()) && Storage.exists(temporary.c_str()) && tryOpen(temporary, true)) return true;

  if (Storage.exists(canonical.c_str()) || Storage.exists(backup.c_str()) || Storage.exists(temporary.c_str())) {
    if (!Storage.exists(canonical.c_str()) && !Storage.exists(backup.c_str()) && Storage.exists(temporary.c_str())) {
      // An interrupted initial creation has no committed progress to preserve.
      // A complete temp was promoted above; discard only an invalid orphan temp.
      if (!Storage.remove(temporary.c_str())) {
        error = Error::Io;
        return false;
      }
    } else {
      if (error == Error::None) error = Error::InvalidChecksum;
      return false;
    }
  }

  if (Storage.exists(canonical.c_str()) || Storage.exists(backup.c_str())) {
    if (error == Error::None) error = Error::InvalidChecksum;
    return false;
  }

  HalFile tempFile = Storage.open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!tempFile) {
    error = Error::Io;
    return false;
  }
  HalFileAccess tempAccess(tempFile);
  StateStore freshStore;
  const bool initialized = freshStore.initialize(tempAccess, packageCrc, cardCount, error);
  tempFile.close();
  if (!initialized) {
    Storage.remove(temporary.c_str());
    return false;
  }
  return tryOpen(temporary, true);
}

bool PersistentState::load(const uint32_t cardIndex, const uint64_t cardId, CardState& state, Error& error) {
  if (!opened_) {
    error = Error::Io;
    return false;
  }
  return store_.load(access_, cardIndex, cardId, state, error);
}

bool PersistentState::save(const uint32_t cardIndex, const CardState& state, Error& error) {
  if (!opened_) {
    error = Error::Io;
    return false;
  }
  return store_.save(access_, cardIndex, state, error);
}

bool loadCheckpoint(const std::string& packagePath, const uint32_t packageCrc, const uint32_t cardCount,
                    SessionCheckpoint& checkpoint) {
  std::string canonical, temporary, backup;
  if (!checkpointPaths(packagePath, canonical, temporary, backup)) return false;
  const std::string damaged = canonical + ".bad";
  const bool canonicalExists = Storage.exists(canonical.c_str());
  SessionCheckpoint canonicalCandidate, backupCandidate, temporaryCandidate, damagedCandidate;
  const bool canonicalValid = canonicalExists &&
                              readCheckpointFile(canonical, packageCrc, cardCount, canonicalCandidate);
  const bool backupValid = Storage.exists(backup.c_str()) &&
                           readCheckpointFile(backup, packageCrc, cardCount, backupCandidate);
  const bool temporaryValid = Storage.exists(temporary.c_str()) &&
                              readCheckpointFile(temporary, packageCrc, cardCount, temporaryCandidate);
  const bool damagedValid = Storage.exists(damaged.c_str()) &&
                            readCheckpointFile(damaged, packageCrc, cardCount, damagedCandidate);
  const auto source = chooseCheckpointSource(canonicalExists, canonicalValid, backupValid, temporaryValid, damagedValid);
  if (source == CheckpointSource::Canonical) {
    checkpoint = canonicalCandidate;
    return true;
  }
  const auto preserveCanonical = [&]() {
    if (!Storage.exists(canonical.c_str())) return true;
    if (damagedValid) return Storage.remove(canonical.c_str());
    if (Storage.exists(damaged.c_str()) && !Storage.remove(damaged.c_str())) return false;
    return Storage.rename(canonical.c_str(), damaged.c_str());
  };
  if (source == CheckpointSource::Backup || source == CheckpointSource::Temporary) {
    if (!preserveCanonical()) return false;
    const std::string& sourcePath = source == CheckpointSource::Temporary ? temporary : backup;
    if (!Storage.rename(sourcePath.c_str(), canonical.c_str())) return false;
    checkpoint = source == CheckpointSource::Temporary ? temporaryCandidate : backupCandidate;
    return true;
  }
  if (source == CheckpointSource::Damaged) {
    if (Storage.exists(canonical.c_str()) && !Storage.remove(canonical.c_str())) return false;
    if (!Storage.rename(damaged.c_str(), canonical.c_str())) return false;
    checkpoint = damagedCandidate;
    return true;
  }
  if (canonicalExists && !canonicalValid && !preserveCanonical()) return false;
  return false;
}

bool saveCheckpoint(const std::string& packagePath, const SessionCheckpoint& checkpoint) {
  std::string canonical, temporary, backup;
  if (!checkpointPaths(packagePath, canonical, temporary, backup)) return false;
  std::array<uint8_t, CHECKPOINT_BYTES> bytes{};
  if (!encodeCheckpoint(checkpoint, bytes.data(), bytes.size())) return false;
  const std::string damaged = canonical + ".bad";
  const bool hasCandidates = Storage.exists(canonical.c_str()) || Storage.exists(temporary.c_str()) ||
                             Storage.exists(backup.c_str()) || Storage.exists(damaged.c_str());
  if (hasCandidates) {
    SessionCheckpoint previous;
    if (!loadCheckpoint(packagePath, checkpoint.packageCrc, checkpoint.cardCount, previous)) {
      CheckpointFileStatus status = CheckpointFileStatus::Missing;
      const std::string paths[] = {canonical, temporary, backup, damaged};
      for (const auto& path : paths) {
        SessionCheckpoint candidate;
        const auto candidateStatus = readCheckpointStatus(path, checkpoint.packageCrc, checkpoint.cardCount, candidate);
        if (candidateStatus == CheckpointFileStatus::Valid || candidateStatus == CheckpointFileStatus::IoError) return false;
        if (candidateStatus == CheckpointFileStatus::Invalid) status = candidateStatus;
      }
      if (status == CheckpointFileStatus::Invalid) {
        // Every extant candidate was read completely and rejected for this
        // package identity/checksum; only then is it safe to discard these
        // stale/corrupt checkpoints and start a new transaction.
        for (const auto& path : paths) {
          if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) return false;
        }
      }
    }
  }
  if (Storage.exists(temporary.c_str()) && !Storage.remove(temporary.c_str())) return false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("STUDY", temporary, file)) return false;
    if (file.write(bytes.data(), bytes.size()) != bytes.size()) return false;
    file.flush();
    if (!file.close()) return false;
  }
  SessionCheckpoint verified;
  if (!readCheckpointFile(temporary, checkpoint.packageCrc, checkpoint.cardCount, verified)) {
    Storage.remove(temporary.c_str());
    return false;
  }

  if (Storage.exists(canonical.c_str())) {
    SessionCheckpoint previous;
    if (!readCheckpointFile(canonical, checkpoint.packageCrc, checkpoint.cardCount, previous)) {
      // Keep an invalid checkpoint intact for diagnosis; do not replace a file
      // whose state could not be validated before the transaction.
      Storage.remove(temporary.c_str());
      return false;
    }
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
    if (Storage.exists(backup.c_str())) Storage.rename(backup.c_str(), canonical.c_str());
    Storage.remove(temporary.c_str());
    return false;
  }
  if (Storage.exists(backup.c_str())) Storage.remove(backup.c_str());
  if (Storage.exists(damaged.c_str()) && !Storage.remove(damaged.c_str())) return false;
  return true;
}

bool clearCheckpoint(const std::string& packagePath) {
  std::string canonical, temporary, backup;
  if (!checkpointPaths(packagePath, canonical, temporary, backup)) return false;
  bool ok = true;
  if (Storage.exists(canonical.c_str())) ok = Storage.remove(canonical.c_str()) && ok;
  if (Storage.exists(temporary.c_str())) ok = Storage.remove(temporary.c_str()) && ok;
  if (Storage.exists(backup.c_str())) ok = Storage.remove(backup.c_str()) && ok;
  const std::string damaged = canonical + ".bad";
  if (Storage.exists(damaged.c_str())) ok = Storage.remove(damaged.c_str()) && ok;
  return ok;
}

}  // namespace Study
