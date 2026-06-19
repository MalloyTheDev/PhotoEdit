#include "pe/core/ColorOps.hpp"

#include "pe/core/Brush.hpp"  // PaintCommand
#include "pe/core/ColorTransform.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"  // bakePixelEdit
#include "pe/core/GroupLayer.hpp"

#include <lcms2.h>

#include <mutex>
#include <vector>

namespace pe {

namespace {

cmsUInt32Number lcmsIntent(RenderingIntent intent) noexcept {
    switch (intent) {
        case RenderingIntent::Perceptual:
            return INTENT_PERCEPTUAL;
        case RenderingIntent::Saturation:
            return INTENT_SATURATION;
        case RenderingIntent::AbsoluteColorimetric:
            return INTENT_ABSOLUTE_COLORIMETRIC;
        case RenderingIntent::RelativeColorimetric:
        default:
            return INTENT_RELATIVE_COLORIMETRIC;
    }
}

// Collect the ids of every pixel layer in the tree (recursing into groups).
void collectPixelLayerIds(const Layer* layer, std::vector<LayerId>& out) {
    if (layer == nullptr) return;
    if (layer->kind() == LayerKind::Pixel) {
        out.push_back(layer->id());
    } else if (layer->kind() == LayerKind::Group) {
        const auto* group = static_cast<const GroupLayer*>(layer);
        for (const auto& child : group->children()) collectPixelLayerIds(child.get(), out);
    }
}

// Converts the document: applies the pre-built per-layer pixel edits and re-tags the
// profile as one reversible unit. The pixel edits are PaintCommands (tile-delta undo,
// native bit depth); the profile swap restores on undo.
class ConvertProfileCommand final : public Command {
public:
    ConvertProfileCommand(std::vector<std::unique_ptr<PaintCommand>> edits, ColorProfileRef from,
                          ColorProfileRef to)
        : edits_(std::move(edits)), from_(std::move(from)), to_(std::move(to)) {}

    [[nodiscard]] std::string name() const override { return "Convert Profile"; }

    DocumentChange execute(Document& doc) override {
        for (auto& edit : edits_) edit->execute(doc);
        doc.cmdSetColorProfile(
            to_);  // pure mutator; the returned Pixels change is the notification
        // The empty dirtyRegion makes the renderer invalidate the whole cache, so the converted
        // pixels (and the re-tagged appearance) recomposite — no separate Profile notify needed.
        return DocumentChange{DocumentChange::Kind::Pixels, Rect{}, kNoLayer};
    }

    DocumentChange undo(Document& doc) override {
        for (auto it = edits_.rbegin(); it != edits_.rend(); ++it) (*it)->undo(doc);
        doc.cmdSetColorProfile(
            from_);  // pure mutator; the returned Pixels change is the notification
        return DocumentChange{DocumentChange::Kind::Pixels, Rect{}, kNoLayer};
    }

private:
    std::vector<std::unique_ptr<PaintCommand>> edits_;
    ColorProfileRef from_;
    ColorProfileRef to_;
};

}  // namespace

std::unique_ptr<Command> convertToProfile(Document& doc, ColorProfileRef target,
                                          RenderingIntent intent, bool blackPointCompensation) {
    ColorProfileRef source = doc.colorProfile();
    if (!source || !target) return nullptr;  // need both a source tag and a destination

    ColorTransformRef xform =
        ColorTransform::create(*source, *target, intent, blackPointCompensation);
    if (!xform) return nullptr;  // non-RGB profile, lcms failure, etc.

    // Build a per-layer pixel edit for every pixel layer (each baked from its current
    // content at its native depth). bakePixelEdit returns nullptr for empty layers.
    std::vector<LayerId> ids;
    for (const auto& top : doc.topLevelLayers()) collectPixelLayerIds(top.get(), ids);

    const auto applyXform = [&xform](std::span<Rgbaf> img, int, int) { xform->applyInPlace(img); };
    std::vector<std::unique_ptr<PaintCommand>> edits;
    for (const LayerId id : ids) {
        if (auto cmd = bakePixelEdit(doc, id, "Convert Profile", applyXform, nullptr)) {
            edits.push_back(std::move(cmd));
        }
    }

    // Even with no content to transform, Convert still re-tags the document.
    return std::make_unique<ConvertProfileCommand>(std::move(edits), std::move(source),
                                                   std::move(target));
}

PixelBuffer convertForDisplay(const PixelBufferF& working, const ColorProfileRef& workingProfile,
                              const ColorProfileRef& displayProfile, ColorEngine& engine,
                              RenderingIntent intent, bool blackPointCompensation) {
    PixelBuffer out(working.width(), working.height());
    if (working.isEmpty()) return out;

    const std::size_t n =
        static_cast<std::size_t>(working.width()) * static_cast<std::size_t>(working.height());

    ColorTransformRef xform;
    if (workingProfile && displayProfile) {
        xform = engine.transform(workingProfile, displayProfile, intent, blackPointCompensation);
    }

    if (xform) {
        // Transform the working pixels into display space, then quantize to 8-bit.
        std::vector<Rgbaf> buf(working.data(), working.data() + n);
        xform->applyInPlace(buf);
        for (std::size_t i = 0; i < n; ++i) out.data()[i] = toRgba8(buf[i]);
    } else {
        // No color management available: present the working values directly.
        for (std::size_t i = 0; i < n; ++i) out.data()[i] = toRgba8(working.data()[i]);
    }
    return out;
}

PixelBuffer convertForProof(const PixelBufferF& working, const ColorProfileRef& workingProfile,
                            const ColorProfileRef& displayProfile,
                            const ColorProfileRef& proofProfile, RenderingIntent intent,
                            RenderingIntent proofIntent, bool blackPointCompensation,
                            bool gamutCheck, Rgbaf gamutAlarm) {
    PixelBuffer out(working.width(), working.height());
    if (working.isEmpty()) return out;

    const std::size_t n =
        static_cast<std::size_t>(working.width()) * static_cast<std::size_t>(working.height());

    // Build the proofing transform with FLOAT input and 8-BIT output directly: lcms2's
    // gamut-check alarm codes are honored for integer output (not float), and the
    // result IS the 8-bit display raster. proof may be any space; working/display RGB.
    cmsHTRANSFORM t = nullptr;
    if (workingProfile && displayProfile && proofProfile &&
        workingProfile->mode() == ColorMode::RGB && displayProfile->mode() == ColorMode::RGB) {
        cmsUInt32Number flags = cmsFLAGS_COPY_ALPHA | cmsFLAGS_SOFTPROOFING;
        if (gamutCheck) flags |= cmsFLAGS_GAMUTCHECK;
        if (blackPointCompensation) flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;

        // cmsSetAlarmCodes is process-GLOBAL state baked into the transform at creation.
        // Serialize the set+build so a concurrent proofing call (ColorEngine advertises
        // thread-safe transform sharing) can't change the alarm color in between.
        static std::mutex alarmMutex;
        const std::lock_guard<std::mutex> lock(alarmMutex);
        if (gamutCheck) {
            cmsUInt16Number alarm[cmsMAXCHANNELS] = {0};
            alarm[0] = static_cast<cmsUInt16Number>(clamp01(gamutAlarm.r) * 65535.0f);
            alarm[1] = static_cast<cmsUInt16Number>(clamp01(gamutAlarm.g) * 65535.0f);
            alarm[2] = static_cast<cmsUInt16Number>(clamp01(gamutAlarm.b) * 65535.0f);
            cmsSetAlarmCodes(alarm);
        }
        t = cmsCreateProofingTransform(
            static_cast<cmsHPROFILE>(workingProfile->nativeHandle()), TYPE_RGBA_FLT,
            static_cast<cmsHPROFILE>(displayProfile->nativeHandle()), TYPE_RGBA_8,
            static_cast<cmsHPROFILE>(proofProfile->nativeHandle()), lcmsIntent(intent),
            lcmsIntent(proofIntent), flags);
    }

    if (t != nullptr) {
        const Rgbaf* src = working.data();
        Rgba8* dst = out.data();
        std::size_t remaining = n;
        constexpr std::size_t kChunk = 0x4000000;  // 64M pixels per cmsDoTransform call
        while (remaining > 0) {
            const std::size_t c = remaining < kChunk ? remaining : kChunk;
            cmsDoTransform(t, src, dst, static_cast<cmsUInt32Number>(c));
            src += c;
            dst += c;
            remaining -= c;
        }
        cmsDeleteTransform(t);
    } else {
        // No color management available: present the working values directly.
        for (std::size_t i = 0; i < n; ++i) out.data()[i] = toRgba8(working.data()[i]);
    }
    return out;
}

}  // namespace pe
