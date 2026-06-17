#pragma once

#include "pe/core/Tile.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace pe {

// Simple RAM budget tracker and scratch pager skeleton (per docs/systems/22-performance.md).
// For M2+ skeleton: display cache uses it; layer TileStores can be extended later.
// Supports basic eviction + spill to temp files (zlib if available, else raw).
// Deterministic, no Qt.

class ScratchDisk {
public:
    explicit ScratchDisk();
    ~ScratchDisk();

    // Spill a tile's raw bytes (compressed if possible) to disk.
    void spill(TileCoord key, std::span<const std::byte> data);

    // Fault a tile back; returns false if not found / error.
    [[nodiscard]] bool fault(TileCoord key, std::vector<std::byte>& out);

    void clear();  // for tests / reset

private:
    std::filesystem::path scratchRoot_;
    std::map<std::pair<int,int>, std::filesystem::path> spilled_;
};

class Performance {
public:
    static Performance& instance();  // singleton for simplicity in skeleton

    void setRAMBudgetBytes(size_t bytes) noexcept;
    [[nodiscard]] size_t ramBudgetBytes() const noexcept { return budgetBytes_; }

    // Called by caches when considering eviction.
    [[nodiscard]] bool overBudget(size_t currentBytes) const noexcept;

    ScratchDisk& scratch() { return scratch_; }

private:
    Performance();
    size_t budgetBytes_ = 128ull * 1024 * 1024;  // 128 MB default
    ScratchDisk scratch_;
};

} // namespace pe
