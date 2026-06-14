#include "pe/core/ColorOps.hpp"

#include "pe/core/Brush.hpp"  // PaintCommand
#include "pe/core/ColorTransform.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"  // bakePixelEdit
#include "pe/core/GroupLayer.hpp"

#include <vector>

namespace pe {

namespace {

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
        doc.cmdSetColorProfile(to_);
        return DocumentChange{DocumentChange::Kind::Pixels, Rect{}, kNoLayer};
    }

    DocumentChange undo(Document& doc) override {
        for (auto it = edits_.rbegin(); it != edits_.rend(); ++it) (*it)->undo(doc);
        doc.cmdSetColorProfile(from_);
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

}  // namespace pe
