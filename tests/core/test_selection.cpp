#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <vector>

using namespace pe;

namespace {
int alphaAt(const Document& doc, LayerId id, int x, int y) {
    const auto* pl = static_cast<const PixelLayer*>(doc.findLayer(id));
    return pl->tiles().pixel(x, y).a;
}
}  // namespace

PE_TEST(selection_inactive_is_all_editable) {
    Selection s;
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
    PE_CHECK_NEAR(s.coverage(-100, 9999), 1.0f);
    PE_CHECK_EQ(s.value(5, 5), static_cast<uint8_t>(255));
}

PE_TEST(selection_rect) {
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    PE_CHECK(s.active());
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);    // inside
    PE_CHECK_NEAR(s.coverage(20, 20), 0.0f);  // outside
    PE_CHECK_EQ(s.value(5, 5), static_cast<uint8_t>(255));
    PE_CHECK_EQ(s.value(20, 20), static_cast<uint8_t>(0));

    s.selectNone();
    PE_CHECK(!s.active());
    PE_CHECK_NEAR(s.coverage(20, 20), 1.0f);  // editable again
}

PE_TEST(selection_boolean_ops) {
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    s.addRect(Rect{20, 20, 5, 5});
    PE_CHECK_NEAR(s.coverage(5, 5), 1.0f);
    PE_CHECK_NEAR(s.coverage(22, 22), 1.0f);
    PE_CHECK_NEAR(s.coverage(15, 15), 0.0f);

    Selection sub;
    sub.selectRect(Rect{0, 0, 20, 20});
    sub.subtractRect(Rect{0, 0, 10, 20});
    PE_CHECK_NEAR(sub.coverage(5, 5), 0.0f);   // removed
    PE_CHECK_NEAR(sub.coverage(15, 5), 1.0f);  // kept

    Selection inter;
    inter.selectRect(Rect{0, 0, 20, 20});
    inter.intersectRect(Rect{10, 0, 20, 20});
    PE_CHECK_NEAR(inter.coverage(5, 5), 0.0f);   // outside intersection
    PE_CHECK_NEAR(inter.coverage(15, 5), 1.0f);  // inside intersection
}

PE_TEST(selection_invert) {
    Selection s;
    s.selectRect(Rect{0, 0, 10, 10});
    s.invert(Rect{0, 0, 20, 20});
    PE_CHECK_NEAR(s.coverage(5, 5), 0.0f);    // was selected -> now not
    PE_CHECK_NEAR(s.coverage(15, 15), 1.0f);  // was not -> now selected
}

PE_TEST(paint_is_gated_by_selection) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 64});  // left half only

    BrushSettings b;
    b.diameter = 8;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    std::vector<StrokePoint> pts = {{{4, 32}, 1.0f}, {{60, 32}, 1.0f}};  // crosses the boundary

    auto cmd = paintStroke(*doc, base, b, Rgbaf{1, 0, 0, 1}, pts, &sel);
    PE_CHECK(cmd != nullptr);
    doc->history().push(std::move(cmd));

    PE_CHECK_EQ(alphaAt(*doc, base, 10, 32), 255);  // inside selection -> painted
    PE_CHECK_EQ(alphaAt(*doc, base, 50, 32), 0);    // outside selection -> blocked
}
