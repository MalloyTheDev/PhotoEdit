#pragma once

#include "pe/core/ColorEngine.hpp"
#include "pe/core/ColorProfile.hpp"  // ColorProfileRef, RenderingIntent
#include "pe/core/Command.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <memory>

namespace pe {

class Document;

// Build a reversible command that CONVERTS the document into `target`: transform
// every pixel layer's content from the document's current profile into `target`
// (preserving appearance), then re-tag the document with `target`. Unlike Assign
// (which changes only the interpretation), Convert changes the numbers so the colors
// look the same in the new space.
//
// Returns nullptr if the document is untagged (no source profile), `target` is null,
// or the transform cannot be built (e.g. a non-RGB profile, for now). The returned
// command is NOT applied yet — push it to history. Only available when built with
// lcms2 (PHOTOEDIT_HAVE_LCMS2). See docs/systems/15-color-management.md.
[[nodiscard]] std::unique_ptr<Command> convertToProfile(
    Document& doc, ColorProfileRef target,
    RenderingIntent intent = RenderingIntent::RelativeColorimetric,
    bool blackPointCompensation = true);

// The working->display leg: convert a composited working-space float image into the
// monitor's display profile, returning an 8-bit display-ready raster. The cached
// working->display transform comes from `engine`. If either profile is null or the
// transform can't be built, falls back to a direct 8-bit quantization (no color
// conversion) — so an uncolor-managed document still displays. Only built with lcms2.
[[nodiscard]] PixelBuffer convertForDisplay(
    const PixelBufferF& working, const ColorProfileRef& workingProfile,
    const ColorProfileRef& displayProfile, ColorEngine& engine,
    RenderingIntent intent = RenderingIntent::RelativeColorimetric,
    bool blackPointCompensation = true);

// Soft-proof: convert a composited working-space float image to the display while
// simulating the `proofProfile` output device, returning an 8-bit display raster.
// With gamutCheck, pixels outside the proof gamut are painted `gamutAlarm`. Falls
// back to a direct quantization if any profile is null or the transform can't build.
// Only built with lcms2.
[[nodiscard]] PixelBuffer convertForProof(
    const PixelBufferF& working, const ColorProfileRef& workingProfile,
    const ColorProfileRef& displayProfile, const ColorProfileRef& proofProfile,
    RenderingIntent intent = RenderingIntent::RelativeColorimetric,
    RenderingIntent proofIntent = RenderingIntent::RelativeColorimetric,
    bool blackPointCompensation = true, bool gamutCheck = false,
    Rgbaf gamutAlarm = Rgbaf{0.5f, 0.5f, 0.5f, 1.0f});

}  // namespace pe
