#pragma once

#include <cstdint>
#include <string>

namespace pe {

// A structured account of an operation declining to run.
//
// Refusal used to be expressed as silence: an early return, a `false`, a `nullopt`, or a
// precondition that failed and was never mentioned. From the user's side that is
// indistinguishable from a broken feature, which makes it a trust problem rather than a
// presentation one. The goal this type serves is that NO user action can fail silently.
//
// It carries more than a message on purpose. A message can only be read by a person, once,
// in a status bar; the fields below let a caller decide what to do (is a retry meaningful?
// would changing the selection help?), let a test assert on something stable, and let a
// report be diagnosed without reproducing the situation that produced it.
//
// This lives in the engine rather than the shell so an engine operation can adopt it
// without the type moving. Today the shell is where most refusals are detected, because
// the engine's own refusals mostly surface as a null command that the shell then explains.

// Why the operation declined. MACHINE-STABLE: these values are part of the diagnostic
// contract, so append new ones and never renumber or reuse an existing value.
enum class RefusalCode : std::uint16_t {
    None = 0,

    // Nothing to operate on.
    NoDocument = 1,
    NoActiveLayer = 2,
    NoSelection = 3,

    // There is a target, but it is the wrong kind or in the wrong state.
    LayerNotPixel = 10,
    LayerHasNoMask = 11,
    LayerAlreadyHasMask = 12,
    LayerNotAdjustment = 13,
    LayerNotText = 14,
    LayerNotTopLevel = 15,
    LayerNotGroup = 16,
    LayerIsClipped = 17,

    // Valid, but it would change nothing, so performing it would only add a history entry
    // the user would then have to undo.
    NoEffect = 20,

    // Temporarily invalid because an edit is in flight.
    StrokeInProgress = 30,
    TransformInProgress = 31,

    // The request is outside what the engine will do.
    PointOutsideCanvas = 40,
    OverSizeBudget = 41,

    // This build or this format cannot do it.
    Unsupported = 50,
};

// A coarse grouping, for a caller that wants to treat a whole class alike (an icon, a
// severity, a decision about whether to interrupt). Never a substitute for the code.
enum class RefusalCategory : std::uint8_t {
    NoTarget,     // nothing to act on
    WrongTarget,  // wrong kind of target, or wrong state
    NoEffect,     // would change nothing
    Busy,         // an edit is in flight
    OverBudget,   // beyond an engine limit
    Unsupported,  // this build or format cannot
};

struct Refusal {
    // Stable identifier of the operation, e.g. "select.feather", "layer.mask.add". Dotted
    // and lowercase so it groups sensibly when several are logged together.
    std::string operation;
    RefusalCode code = RefusalCode::None;
    RefusalCategory category = RefusalCategory::NoTarget;

    // What the user did, in their words, e.g. "Select > Feather...". Distinct from
    // `operation`: several affordances can reach one operation, and knowing WHICH one was
    // used is most of a bug report.
    std::string action;

    // Human-readable, and it should name the corrective action rather than only the
    // problem. "Select a pixel layer to paint on" beats "invalid layer".
    std::string explanation;

    // Enough state to diagnose this without reproducing it: what the active layer was,
    // whether a selection existed, the relevant sizes.
    std::string context;

    // Would doing exactly the same thing again succeed? Almost never true; it is here so a
    // caller can tell a transient refusal (an edit in flight) from a structural one.
    bool retryMeaningful = false;

    // Would changing the selection, the active layer, or a setting make this valid?
    // False when no choice of target helps: an unsupported format, and an over-budget
    // operation, which stays over budget whichever layer is picked. The user's way out
    // there is to reduce the work, not to re-aim it.
    bool fixableByState = true;

    [[nodiscard]] bool isRefusal() const noexcept { return code != RefusalCode::None; }
};

// Receives refusals. The shell implements this to render them; a test implements it to
// assert on them.
class RefusalSink {
public:
    virtual ~RefusalSink() = default;
    virtual void onRefused(const Refusal&) = 0;
};

// The category each code belongs to. Kept as one function rather than a field the caller
// must remember to set, so a code cannot be filed under the wrong class by accident.
[[nodiscard]] constexpr RefusalCategory categoryOf(RefusalCode code) noexcept {
    switch (code) {
        case RefusalCode::NoDocument:
        case RefusalCode::NoActiveLayer:
        case RefusalCode::NoSelection:
            return RefusalCategory::NoTarget;
        case RefusalCode::LayerNotPixel:
        case RefusalCode::LayerHasNoMask:
        case RefusalCode::LayerAlreadyHasMask:
        case RefusalCode::LayerNotAdjustment:
        case RefusalCode::LayerNotText:
        case RefusalCode::LayerNotTopLevel:
        case RefusalCode::LayerNotGroup:
        case RefusalCode::LayerIsClipped:
        case RefusalCode::PointOutsideCanvas:
            return RefusalCategory::WrongTarget;
        case RefusalCode::NoEffect:
            return RefusalCategory::NoEffect;
        case RefusalCode::StrokeInProgress:
        case RefusalCode::TransformInProgress:
            return RefusalCategory::Busy;
        case RefusalCode::OverSizeBudget:
            return RefusalCategory::OverBudget;
        case RefusalCode::Unsupported:
            return RefusalCategory::Unsupported;
        case RefusalCode::None:
        default:
            return RefusalCategory::NoTarget;
    }
}

// Build a refusal with its category and its retry semantics derived from the code, so the
// three cannot disagree. Only Busy refusals are worth retrying unchanged, and only an
// Unsupported one is beyond the user's power to fix.
[[nodiscard]] inline Refusal refuse(std::string operation, RefusalCode code, std::string action,
                                    std::string explanation, std::string context = {}) {
    const RefusalCategory category = categoryOf(code);
    return Refusal{.operation = std::move(operation),
                   .code = code,
                   .category = category,
                   .action = std::move(action),
                   .explanation = std::move(explanation),
                   .context = std::move(context),
                   .retryMeaningful = category == RefusalCategory::Busy,
                   .fixableByState = category != RefusalCategory::Unsupported &&
                                     category != RefusalCategory::OverBudget};
}

}  // namespace pe
