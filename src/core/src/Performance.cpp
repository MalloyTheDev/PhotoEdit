#include "pe/core/Performance.hpp"

#include "pe/core/Tile.hpp"

#ifdef PHOTOEDIT_HAVE_ZLIB
#include <zlib.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pe {

namespace {
std::filesystem::path makeScratchDir() {
    auto p = std::filesystem::temp_directory_path() / "pe_scratch";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

std::pair<int,int> tileKey(TileCoord c) { return {c.col, c.row}; }
} // namespace

ScratchDisk::ScratchDisk() : scratchRoot_(makeScratchDir()) {}
ScratchDisk::~ScratchDisk() { clear(); }

void ScratchDisk::spill(TileCoord kcoord, std::span<const std::byte> data) {
    auto k = tileKey(kcoord);
    auto path = scratchRoot_ / (std::to_string(k.first) + "_" + std::to_string(k.second) + ".tile");
    spilled_[k] = path;

#ifdef PHOTOEDIT_HAVE_ZLIB
    uLongf bound = compressBound(static_cast<uLong>(data.size()));
    std::vector<std::byte> comp(bound);
    uLongf outLen = bound;
    if (compress2(reinterpret_cast<Bytef*>(comp.data()), &outLen,
                  reinterpret_cast<const Bytef*>(data.data()), static_cast<uLong>(data.size()),
                  Z_BEST_SPEED) == Z_OK) {
        comp.resize(outLen);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(comp.data()), comp.size());
        return;
    }
#endif
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
}

bool ScratchDisk::fault(TileCoord kcoord, std::vector<std::byte>& out) {
    auto k = tileKey(kcoord);
    auto it = spilled_.find(k);
    if (it == spilled_.end()) return false;
    auto path = it->second;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    f.seekg(0);
    out.resize(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(out.data()), size)) return false;

#ifdef PHOTOEDIT_HAVE_ZLIB
    // Try decompress; if fails, treat as raw
    std::vector<std::byte> decomp(256*256*4); // assume max
    uLongf outLen = decomp.size();
    if (uncompress(reinterpret_cast<Bytef*>(decomp.data()), &outLen,
                   reinterpret_cast<const Bytef*>(out.data()), static_cast<uLong>(out.size())) == Z_OK) {
        out.assign(decomp.begin(), decomp.begin() + outLen);
    }
#endif
    return true;
}

void ScratchDisk::clear() {
    for (auto& [k, p] : spilled_) {
        std::error_code ec; std::filesystem::remove(p, ec);
    }
    spilled_.clear();
}

Performance& Performance::instance() {
    static Performance p;
    return p;
}

Performance::Performance() {}

void Performance::setRAMBudgetBytes(size_t bytes) noexcept {
    budgetBytes_ = bytes ? bytes : 128ull*1024*1024;
}

bool Performance::overBudget(size_t currentBytes) const noexcept {
    return currentBytes > budgetBytes_;
}

} // namespace pe
