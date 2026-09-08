#pragma once

#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"

#include <cstdint>

namespace pe {

class Document;

// Which layer is under a document point.
//
// This exists for the Move tool's Auto-Select: a click on the canvas should be able to
// grab whatever the user can actually see there, rather than requiring them to find the
// right row in the Layers panel first. It answers the same question the compositor
// answers for a whole tile, but for one pixel and with the layer's identity kept.
//
// It walks the stack from the TOP down and returns the first layer that covers the point,
// which is the one drawn nearest the viewer and so the one the user means. "Covers" is the
// layer's own alpha at that point, scaled by its mask, its opacity, and the opacity of
// every group containing it: what reaches the screen from that layer alone. Blend modes
// are deliberately not modelled. A blend mode changes how a layer's colour combines with
// what is beneath it, not whether the layer is present at that pixel, and a Multiply layer
// that reads as white is still the thing the user clicked on.
//
// Skipped: hidden layers, hidden groups, and adjustment layers. An adjustment has no
// pixels of its own (its content bounds are the whole representable plane, so it would
// otherwise swallow every click) and clicking one to move it is not a meaningful gesture.

// The topmost layer whose coverage at `p` is greater than `threshold`, or kNoLayer when
// nothing is there. `threshold` is in [0, 1]; the default rejects only pixels that are
// exactly transparent, which is the boundary a user perceives.
//
// Returns the individual layer, at whatever depth it sits. For Auto-Select's Group
// granularity, pass the result through topLevelAncestorOf().
[[nodiscard]] LayerId layerAt(const Document& doc, Point p, float threshold = 0.0f);

// The top-level layer containing `id`: `id` itself when it is already top level, the
// enclosing top-level group when it is nested, and kNoLayer when it is not in the
// document at all.
[[nodiscard]] LayerId topLevelAncestorOf(const Document& doc, LayerId id);

// Diagnostic: tiles rendered by layerAt() since the process started.
//
// A tile is 256x256 RGBA float, so rendering one to read a single pixel is a megabyte of
// work, and the walk skips layers rather than sampling them wherever it can: a layer whose
// content bounds miss the point, a hidden layer, a clear one, and every adjustment layer,
// whose bounds are the whole representable plane and so reject nothing. None of those
// skips changes the ANSWER, which means nothing else can observe whether they happen. Same
// purpose and same idiom as Filter's moveTileBuildCount.
[[nodiscard]] std::uint64_t hitTestTileRenderCount() noexcept;

}  // namespace pe
