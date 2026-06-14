#pragma once

#include "pe/core/ColorProfile.hpp"  // ColorProfileRef, RenderingIntent
#include "pe/core/Command.hpp"

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

}  // namespace pe
