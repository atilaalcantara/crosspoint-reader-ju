#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "study/StudyPackage.h"

// Browser-only hierarchy. IDs are always scoped to a package, never shared
// across files. The table can be in any order, so traverse parent links rather
// than sorting by depth (which would separate siblings from their children).
class StudyDeckTree {
 public:
  struct Deck {
    Study::DeckInfo info;
    std::vector<size_t> children;
    bool expanded = false;
  };
  struct Package {
    std::string path;
    uint32_t crc = 0;
    std::vector<Deck> decks;
    std::vector<size_t> roots;
  };
  enum class RowKind { Deck, StudySubtree };
  struct Row {
    size_t packageIndex;
    size_t deckIndex;
    uint32_t depth;
    RowKind kind;
    bool packageHeading;
  };

  void clear() { packages_.clear(); }

  void addPackage(std::string path, uint32_t crc, const std::vector<Study::DeckInfo>& decks) {
    Package package;
    package.path = std::move(path);
    package.crc = crc;
    package.decks.reserve(decks.size());
    for (const auto& deck : decks) package.decks.push_back(Deck{deck, {}, false});
    for (size_t i = 0; i < decks.size(); ++i) {
      if (decks[i].parentId == 0) {
        package.roots.push_back(i);
      } else {
        const auto parent = std::find_if(decks.begin(), decks.end(), [&](const auto& candidate) {
          return candidate.id == decks[i].parentId;
        });
        if (parent != decks.end()) package.decks[static_cast<size_t>(parent - decks.begin())].children.push_back(i);
      }
    }
    packages_.push_back(std::move(package));
  }

  const Package& package(size_t index) const { return packages_[index]; }
  const Deck& deck(const Row& row) const { return packages_[row.packageIndex].decks[row.deckIndex]; }
  bool hasChildren(const Row& row) const { return !deck(row).children.empty(); }

  void toggle(const Row& row) {
    if (row.kind != RowKind::Deck || !hasChildren(row)) return;
    auto& expanded = packages_[row.packageIndex].decks[row.deckIndex].expanded;
    expanded = !expanded;
  }

  // A collapsed branch does not consume the row budget. Report truncation only
  // when currently visible rows really cannot fit on the list.
  std::vector<Row> visibleRows(size_t limit, bool& truncated) const {
    std::vector<Row> rows;
    truncated = false;
    for (size_t p = 0; p < packages_.size(); ++p) {
      const auto& pkg = packages_[p];
      struct Pending {
        size_t index;
        uint32_t depth;
        RowKind kind;
      };
      std::vector<Pending> pending;
      for (auto it = pkg.roots.rbegin(); it != pkg.roots.rend(); ++it)
        pending.push_back({*it, 0, RowKind::Deck});
      bool first = true;
      while (!pending.empty()) {
        const auto next = pending.back();
        pending.pop_back();
        if (rows.size() == limit) {
          truncated = true;
          return rows;
        }
        rows.push_back({p, next.index, next.depth, next.kind, first});
        first = false;
        if (next.kind != RowKind::Deck || !pkg.decks[next.index].expanded) continue;
        const auto& children = pkg.decks[next.index].children;
        for (auto it = children.rbegin(); it != children.rend(); ++it)
          pending.push_back({*it, next.depth + 1, RowKind::Deck});
        if (!children.empty()) pending.push_back({next.index, next.depth + 1, RowKind::StudySubtree});
      }
    }
    return rows;
  }

 private:
  std::vector<Package> packages_;
};
