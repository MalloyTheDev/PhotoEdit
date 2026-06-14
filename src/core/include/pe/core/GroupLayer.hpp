#pragma once

#include "pe/core/Layer.hpp"

#include <span>
#include <vector>

namespace pe {

// A group composites its children into an isolated buffer, then is blended down
// as a single layer. In M1 groups are isolated (children blend over transparency
// first); non-isolated/knockout semantics arrive later. Groups nest recursively.
class GroupLayer final : public Layer {
public:
    explicit GroupLayer(std::string name = "Group");

    [[nodiscard]] bool isolated() const noexcept { return isolated_; }
    void setIsolated(bool v) noexcept { isolated_ = v; }

    [[nodiscard]] std::span<const std::unique_ptr<Layer>> children() const noexcept {
        return children_;
    }
    [[nodiscard]] std::size_t childCount() const noexcept { return children_.size(); }

    // Child management (command-facing). Insertions clamp the index into range.
    void addChild(std::unique_ptr<Layer> child);
    void insertChild(std::size_t index, std::unique_ptr<Layer> child);
    // Remove and return the child with this id (searches direct children only),
    // or nullptr if not found.
    [[nodiscard]] std::unique_ptr<Layer> removeChild(LayerId id);
    // Index of a direct child, or npos.
    [[nodiscard]] std::size_t indexOf(LayerId id) const noexcept;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    [[nodiscard]] Layer* findChild(LayerId id) noexcept;

    // Recursive lookup of a layer anywhere in this group's subtree.
    [[nodiscard]] Layer* findDescendant(LayerId id) noexcept;
    [[nodiscard]] const Layer* findDescendant(LayerId id) const noexcept;

    [[nodiscard]] Rect contentBounds() const noexcept override;
    void renderInto(TileCoord coord, std::span<Rgbaf> dst) const override;
    [[nodiscard]] std::unique_ptr<Layer> clone() const override;

private:
    std::vector<std::unique_ptr<Layer>> children_;
    bool isolated_ = true;
};

}  // namespace pe
