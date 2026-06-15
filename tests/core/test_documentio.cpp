#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace pe;

PE_TEST(documentio_format_from_extension) {
    PE_CHECK(formatFromExtension("a.png") == ImageFormat::Png);
    PE_CHECK(formatFromExtension("/path/to/IMAGE.PNG") == ImageFormat::Png);  // case-insensitive
    PE_CHECK(formatFromExtension("x.jpg") == ImageFormat::Jpeg);
    PE_CHECK(formatFromExtension("x.jpeg") == ImageFormat::Jpeg);
    PE_CHECK(formatFromExtension("x.tif") == ImageFormat::Tiff);
    PE_CHECK(formatFromExtension("a.b.tiff") == ImageFormat::Tiff);  // last extension wins
    PE_CHECK(formatFromExtension("x.webp") == ImageFormat::WebP);
    PE_CHECK(formatFromExtension("doc.pedoc") == ImageFormat::Native);
    PE_CHECK(formatFromExtension("noext") == ImageFormat::Unknown);
    PE_CHECK(formatFromExtension("x.gif") == ImageFormat::Unknown);
    PE_CHECK(formatFromExtension("/dir.png/file") == ImageFormat::Unknown);  // dotted dir, no ext
    PE_CHECK(formatFromExtension("/a.b/photo.tiff") == ImageFormat::Tiff);   // ext on the file
}

PE_TEST(documentio_native_always_available_roundtrip) {
    PE_CHECK(formatAvailable(ImageFormat::Native));  // native needs no external codec

    auto doc = Document::createBlank(Size{12, 8});
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 12, 8}, Rgba8{30, 60, 90, 255});

    std::vector<std::byte> bytes = exportDocument(*doc, ImageFormat::Native);
    PE_CHECK(!bytes.empty());
    auto loaded = importDocument(bytes, ImageFormat::Native);
    PE_CHECK(loaded != nullptr);
    PE_CHECK_EQ(loaded->canvasSize().width, 12);
    auto* lpl = dynamic_cast<PixelLayer*>(loaded->findLayer(loaded->activeLayer()));
    PE_CHECK_EQ(lpl->tiles().pixel(5, 5), (Rgba8{30, 60, 90, 255}));
}

PE_TEST(documentio_document_from_image) {
    PixelBuffer img(4, 3, Rgba8{200, 100, 50, 255});
    img.set(1, 1, Rgba8{10, 20, 30, 128});
    auto doc = documentFromImage(img);
    PE_CHECK(doc != nullptr);
    PE_CHECK_EQ(doc->canvasSize().width, 4);
    PE_CHECK_EQ(doc->canvasSize().height, 3);
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    PE_CHECK_EQ(pl->tiles().pixel(0, 0), (Rgba8{200, 100, 50, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(1, 1), (Rgba8{10, 20, 30, 128}));
    PE_CHECK(documentFromImage(PixelBuffer{}) == nullptr);  // empty -> null
}

PE_TEST(documentio_unavailable_or_unknown_is_null) {
    auto doc = Document::createBlank(Size{4, 4});
    PE_CHECK(exportDocument(*doc, ImageFormat::Unknown).empty());
    std::vector<std::byte> junk(16, std::byte{0});
    PE_CHECK(importDocument(junk, ImageFormat::Unknown) == nullptr);
    // A format whose codec isn't built must round-trip to "unavailable", not crash.
    if (!formatAvailable(ImageFormat::Png)) {
        PE_CHECK(exportDocument(*doc, ImageFormat::Png).empty());
        PE_CHECK(importDocument(junk, ImageFormat::Png) == nullptr);
    }
}

PE_TEST(documentio_file_roundtrip_native) {
    // Portable temp path (works on Windows and Linux CI).
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() /
        ("pe_docio_" + std::to_string(reinterpret_cast<std::uintptr_t>(&file)) + ".pedoc");
    const std::string path = file.string();

    auto doc = Document::createBlank(Size{10, 6});
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 10, 6}, Rgba8{70, 140, 210, 255});

    PE_CHECK(saveDocument(*doc, path));
    auto loaded = loadDocument(path);
    PE_CHECK(loaded != nullptr);
    if (loaded != nullptr) {  // guard the deref so a save/load failure reports, not crashes
        PE_CHECK_EQ(loaded->canvasSize().width, 10);
        auto* lpl = dynamic_cast<PixelLayer*>(loaded->findLayer(loaded->activeLayer()));
        if (lpl != nullptr) PE_CHECK_EQ(lpl->tiles().pixel(5, 3), (Rgba8{70, 140, 210, 255}));
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

PE_TEST(documentio_load_missing_or_unknown_is_null) {
    const std::string missing =
        (std::filesystem::temp_directory_path() / "pe_does_not_exist_98765.pedoc").string();
    const std::string unknownExt =
        (std::filesystem::temp_directory_path() / "whatever.xyz").string();
    PE_CHECK(loadDocument(missing) == nullptr);     // missing file
    PE_CHECK(loadDocument(unknownExt) == nullptr);  // unknown extension

    auto doc = Document::createBlank(Size{4, 4});
    PE_CHECK(!saveDocument(*doc, unknownExt));  // unknown extension -> false
}

#ifdef PHOTOEDIT_HAVE_PNG
PE_TEST(documentio_png_dispatch_roundtrip) {
    PE_CHECK(formatAvailable(ImageFormat::Png));
    auto doc = Document::createBlank(Size{6, 4});
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 6, 4}, Rgba8{12, 34, 56, 255});

    std::vector<std::byte> bytes = exportDocument(*doc, ImageFormat::Png);
    PE_CHECK(!bytes.empty());
    auto loaded = importDocument(bytes, ImageFormat::Png);  // PNG flattens to one layer
    PE_CHECK(loaded != nullptr);
    auto* lpl = dynamic_cast<PixelLayer*>(loaded->findLayer(loaded->activeLayer()));
    PE_CHECK_EQ(lpl->tiles().pixel(3, 2), (Rgba8{12, 34, 56, 255}));
}
#endif
