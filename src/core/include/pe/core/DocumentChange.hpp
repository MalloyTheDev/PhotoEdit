#pragma once

#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"

#include <cstdint>

namespace pe {

// Typed description of what changed, delivered to observers after a committed
// mutation (and after non-undoable session changes like the active layer). It
// ships a description, never pixels: observers react (recomposite, refresh a
// panel) downstream. See docs/systems/01-document-system.md.
struct DocumentChange {
    enum class Kind : uint8_t {
        Pixels,          // a region of one or more layers changed
        LayerStructure,  // add / remove / reorder / group
        LayerProps,      // opacity / blend / visibility / name / lock
        ActiveLayer,     // active-layer session change (not undoable)
        DirtyState,      // saved/unsaved transition
        Profile,         // document color profile assigned/converted (recomposite)
    };

    Kind kind = Kind::Pixels;
    Rect dirtyRegion{};        // meaningful for Pixels: union of touched tiles
    LayerId layer = kNoLayer;  // affected layer where meaningful
};

}  // namespace pe
