// #165: raster export must work above the 64 MP composite cap by streaming the encode band by
// band, so the whole flattened image never exists at once. These pin that a document over the cap
// now exports to PNG and TIFF, and that the bands assemble into the correct image (a round-trip
// through a document tall enough to span more than one band).

#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>

#if defined(PHOTOEDIT_HAVE_PNG) && defined(PHOTOEDIT_HAVE_TIFF)

using namespace pe;

namespace {

PixelLayer* base(Document& doc) {
    return static_cast<PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

// The PNG IHDR width/height, big-endian, at fixed offsets after the 8-byte signature: length(4) +
// "IHDR"(4) then width(4) at offset 16, height(4) at offset 20. Lets the test check the encoded
// dimensions without decoding the (over-cap) pixels.
std::uint32_t be32(const std::byte* p) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(p[3]));
}

}  // namespace

PE_TEST(export_over_the_composite_cap_streams_to_png_and_tiff) {
    // 8200 x 8200 is 67.2 MP, over the 64 MP composite cap. Before streaming, both of these
    // returned empty (the whole flatten was refused); now they must produce a real file.
    auto doc = Document::createBlank(Size{8200, 8200});
    base(*doc)->tiles().fillRect(Rect{0, 0, 256, 256}, Rgba8{200, 100, 50, 255});  // some content

    const std::vector<std::byte> png = exportDocument(*doc, ImageFormat::Png);
    PE_REQUIRE(png.size() > 24);
    PE_CHECK_EQ(static_cast<int>(be32(png.data() + 16)), 8200);  // IHDR width
    PE_CHECK_EQ(static_cast<int>(be32(png.data() + 20)), 8200);  // IHDR height

    const std::vector<std::byte> tiff = exportDocument(*doc, ImageFormat::Tiff);
    PE_CHECK(tiff.size() > 1000);  // a real TIFF, not an empty refusal
}

PE_TEST(export_bands_assemble_the_image_correctly) {
    // 3000 x 3000 (9 MP) exceeds the ~8 MP band budget, so the encode spans more than one band.
    // The top half is red and the bottom half blue; a band-offset bug would misplace the seam, so
    // reading the far-bottom pixel back as blue proves the second band landed at the right rows.
    auto doc = Document::createBlank(Size{3000, 3000});
    base(*doc)->tiles().fillRect(Rect{0, 0, 3000, 1500}, Rgba8{220, 40, 40, 255});  // top red
    base(*doc)->tiles().fillRect(Rect{0, 1500, 3000, 1500},
                                 Rgba8{40, 60, 220, 255});  // bottom blue
    // A distinct thin stripe in the interior of each band, so a bug that writes a band's rows at
    // the wrong y (or writes one row for the whole band) misplaces a marker rather than landing in
    // a uniform field that hides it.
    base(*doc)->tiles().fillRect(Rect{0, 500, 3000, 10}, Rgba8{40, 200, 40, 255});    // green
    base(*doc)->tiles().fillRect(Rect{0, 2700, 3000, 10}, Rgba8{230, 30, 210, 255});  // magenta

    const auto checkRoundTrip = [](const std::optional<PixelBuffer>& r) {
        PE_REQUIRE(r.has_value());
        PE_CHECK_EQ(r->width(), 3000);
        PE_CHECK_EQ(r->height(), 3000);
        PE_CHECK_EQ(r->at(10, 100), (Rgba8{220, 40, 40, 255}));   // band 1: red
        PE_CHECK_EQ(r->at(10, 505), (Rgba8{40, 200, 40, 255}));   // band 1 interior: green stripe
        PE_CHECK_EQ(r->at(10, 2000), (Rgba8{40, 60, 220, 255}));  // band 2: blue
        PE_CHECK_EQ(r->at(10, 2705),
                    (Rgba8{230, 30, 210, 255}));  // band 2 interior: magenta stripe
    };

    checkRoundTrip(decodePng(exportDocument(*doc, ImageFormat::Png)));
    checkRoundTrip(decodeTiff(exportDocument(*doc, ImageFormat::Tiff)));
}

#endif  // PHOTOEDIT_HAVE_PNG && PHOTOEDIT_HAVE_TIFF
