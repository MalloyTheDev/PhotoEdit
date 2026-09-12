#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

#ifdef PHOTOEDIT_HAVE_JPEG
PE_TEST(documentio_export_jpeg_quality_option) {
    // A varied image so JPEG quality has something to trade off (a flat fill compresses
    // to nearly the same size at any quality).
    auto doc = Document::createBlank(Size{64, 64});
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            pl->tiles().setPixel(x, y,
                                 Rgba8{static_cast<uint8_t>(x * 4), static_cast<uint8_t>(y * 4),
                                       static_cast<uint8_t>((x ^ y) * 3), 255});
        }
    }

    const std::vector<std::byte> low = exportDocument(*doc, ImageFormat::Jpeg, ExportOptions{15});
    const std::vector<std::byte> high = exportDocument(*doc, ImageFormat::Jpeg, ExportOptions{95});
    PE_CHECK(!low.empty() && !high.empty());
    PE_CHECK(low.size() < high.size());  // lower quality -> smaller JPEG

    // The no-options overload matches the default options (jpegQuality 90), since both
    // delegate to the same encoder default.
    const std::vector<std::byte> deflt = exportDocument(*doc, ImageFormat::Jpeg);
    const std::vector<std::byte> opt90 = exportDocument(*doc, ImageFormat::Jpeg, ExportOptions{90});
    PE_CHECK_EQ(deflt.size(), opt90.size());
}
#endif

// ---------------------------------------------------------------------------
// Crash-safe saving. docs/systems/20-file-io.md requires that a save "write to a
// temp file and atomically rename; never truncate the user's existing file on a
// failed save", and lists "simulated write failures leave the prior file intact"
// as a test. saveDocument previously opened the destination with std::ios::trunc,
// so any failure mid-write destroyed a file the user had already saved.
// ---------------------------------------------------------------------------

namespace {

// A private directory per test, so a stray-temp scan cannot see another test's files.
std::filesystem::path makeScratchDir(const char* tag) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("pe_savesafe_" + std::string(tag) + "_" +
                                       std::to_string(reinterpret_cast<std::uintptr_t>(tag)));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::size_t countLeftoverTemps(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().filename().string().find(".pe-save-") != std::string::npos) ++n;
    }
    return n;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::unique_ptr<Document> smallDoc() {
    auto doc = Document::createBlank(Size{8, 8});
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    if (pl != nullptr) pl->tiles().fillRect(Rect{0, 0, 8, 8}, Rgba8{1, 2, 3, 255});
    return doc;
}

}  // namespace

PE_TEST(documentio_save_replaces_existing_file_and_leaves_no_temp) {
    const std::filesystem::path dir = makeScratchDir("ok");
    const std::filesystem::path dst = dir / "target.pedoc";
    {
        std::ofstream seed(dst, std::ios::binary | std::ios::trunc);
        seed << "PREVIOUS CONTENT";
    }

    PE_CHECK(saveDocument(*smallDoc(), dst.string()));
    PE_CHECK(readAll(dst) != "PREVIOUS CONTENT");  // actually replaced
    PE_CHECK(loadDocument(dst.string()) != nullptr);
    PE_CHECK_EQ(countLeftoverTemps(dir), static_cast<std::size_t>(0));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

PE_TEST(documentio_failed_save_leaves_the_existing_file_intact) {
    // .psd is import-only, so exportDocument yields nothing and the save fails. The
    // file already at that path must survive untouched.
    const std::filesystem::path dir = makeScratchDir("encodefail");
    const std::filesystem::path dst = dir / "target.psd";
    {
        std::ofstream seed(dst, std::ios::binary | std::ios::trunc);
        seed << "IRREPLACEABLE";
    }

    PE_CHECK(!saveDocument(*smallDoc(), dst.string()));
    PE_CHECK_EQ(readAll(dst), std::string("IRREPLACEABLE"));
    PE_CHECK_EQ(countLeftoverTemps(dir), static_cast<std::size_t>(0));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

PE_TEST(documentio_save_that_cannot_replace_the_destination_preserves_it) {
    // A directory sitting at the destination path makes the final rename fail, which
    // exercises the failure AFTER the temp has been fully written. The destination
    // and its contents must survive, and the temp must not be left behind.
    const std::filesystem::path dir = makeScratchDir("renamefail");
    const std::filesystem::path dst = dir / "target.pedoc";  // created as a DIRECTORY
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    {
        std::ofstream inside(dst / "keepme.txt", std::ios::binary | std::ios::trunc);
        inside << "STILL HERE";
    }

    PE_CHECK(!saveDocument(*smallDoc(), dst.string()));
    PE_CHECK(std::filesystem::is_directory(dst));
    PE_CHECK_EQ(readAll(dst / "keepme.txt"), std::string("STILL HERE"));
    PE_CHECK_EQ(countLeftoverTemps(dir), static_cast<std::size_t>(0));

    std::filesystem::remove_all(dir, ec);
}

PE_TEST(documentio_save_into_a_missing_directory_fails_cleanly) {
    // The temp cannot even be created. Nothing should be produced anywhere.
    const std::filesystem::path dir = makeScratchDir("nodir");
    const std::filesystem::path dst = dir / "no_such_subdir" / "target.pedoc";

    PE_CHECK(!saveDocument(*smallDoc(), dst.string()));
    PE_CHECK(!std::filesystem::exists(dst));
    PE_CHECK_EQ(countLeftoverTemps(dir), static_cast<std::size_t>(0));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

PE_TEST(documentio_save_replaces_the_destination_rather_than_writing_into_it) {
    // The requirement is a temp file plus an atomic rename, NOT a write into the
    // existing file. A hard link makes the difference observable: after a rename the
    // link still names the original file object, so it keeps the old bytes. Had the
    // save truncated and rewritten the destination in place, the link would show the
    // new bytes, because it would be the very same file.
    //
    // This is the case that distinguishes the two implementations. The other
    // crash-safety tests here pin the contract but pass either way, because every
    // failure they can portably trigger happens before a truncating implementation
    // would have opened the destination.
    const std::filesystem::path dir = makeScratchDir("identity");
    const std::filesystem::path dst = dir / "target.pedoc";
    const std::filesystem::path link = dir / "hardlink.bin";
    {
        std::ofstream seed(dst, std::ios::binary | std::ios::trunc);
        seed << "PREVIOUS CONTENT";
    }

    std::error_code ec;
    std::filesystem::create_hard_link(dst, link, ec);
    if (ec) {
        // Some filesystems have no hard links; nothing to assert there.
        std::printf("    (skipped: hard links unsupported here: %s)\n", ec.message().c_str());
        std::filesystem::remove_all(dir, ec);
        return;
    }

    PE_CHECK(saveDocument(*smallDoc(), dst.string()));
    PE_CHECK(readAll(dst) != "PREVIOUS CONTENT");                 // destination updated
    PE_CHECK_EQ(readAll(link), std::string("PREVIOUS CONTENT"));  // original object untouched
    PE_CHECK_EQ(countLeftoverTemps(dir), static_cast<std::size_t>(0));

    std::filesystem::remove_all(dir, ec);
}

PE_TEST(export_over_the_cap_refuses_only_the_flattening_formats) {
    // compositeImage() returns nothing above kMaxCompositeImagePixels. PNG and TIFF no longer go
    // through it: they stream the encode band by band (#165), so they succeed at the project's
    // target sizes. The formats that still flatten the whole image refuse rather than emit a
    // truncated file, because saveDocument() only knows the export failed if the bytes come back
    // empty.
    const int64_t side = 9000;  // 81 MP, over the 64 MP cap
    PE_CHECK(side * side > kMaxCompositeImagePixels);
    auto doc = Document::createBlank(Size{static_cast<int>(side), static_cast<int>(side)});
    PE_CHECK(doc != nullptr);
    PE_CHECK(doc->compositeImage().isEmpty());  // the one-shot flatten still refuses over the cap

#ifdef PHOTOEDIT_HAVE_PNG
    PE_CHECK(!exportDocument(*doc, ImageFormat::Png).empty());  // streams
#endif
#ifdef PHOTOEDIT_HAVE_TIFF
    PE_CHECK(!exportDocument(*doc, ImageFormat::Tiff).empty());  // streams
#endif
    // JPEG (TurboJPEG is one-shot) and WebP still flatten, so they return empty over the cap. A
    // codec not built into this binary also returns empty, the same contract, so these hold in
    // every lane.
    PE_CHECK(exportDocument(*doc, ImageFormat::Jpeg).empty());
    PE_CHECK(exportDocument(*doc, ImageFormat::WebP).empty());

    // The native format serializes tiles directly and so never had this limit.
    PE_CHECK(!exportDocument(*doc, ImageFormat::Native).empty());
}

PE_TEST(loaddocument_reports_why_it_failed) {
    // Six distinct failures used to collapse into a single nullptr, so a user denied read
    // access got the same message as one opening a corrupt file. Only the first is
    // recoverable by the user, and only if they are told which one it is.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pe_loaderr_test";
    std::error_code ec;
    fs::create_directories(dir, ec);

    LoadError err = LoadError::None;

    // Unknown extension: rejected before the file is even looked for.
    PE_CHECK(loadDocument((dir / "x.qqq").string(), &err) == nullptr);
    PE_CHECK(err == LoadError::UnsupportedFormat);

    // A recognized extension that is simply not there.
    err = LoadError::None;
    PE_CHECK(loadDocument((dir / "missing.png").string(), &err) == nullptr);
    PE_CHECK(err == LoadError::NotFound);

    // Present and readable, but the contents are not a PNG.
    const fs::path junk = dir / "junk.png";
    {
        std::ofstream f(junk, std::ios::binary);
        const char* text = "this is definitely not a png";
        f.write(text, 28);
    }
    err = LoadError::None;
    PE_CHECK(loadDocument(junk.string(), &err) == nullptr);
    PE_CHECK(err == LoadError::DecodeFailed);

    // A successful load must leave the code alone rather than reporting a stale failure.
    const fs::path good = dir / "good.pedoc";
    auto doc = Document::createBlank(Size{8, 8});
    PE_CHECK(saveDocument(*doc, good.string()));
    err = LoadError::DecodeFailed;  // deliberately dirty
    auto back = loadDocument(good.string(), &err);
    PE_CHECK(back != nullptr);
    PE_CHECK(err == LoadError::None);

    fs::remove_all(dir, ec);
}

PE_TEST(savedocument_reports_why_it_failed) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pe_saveerr_test";
    std::error_code ec;
    fs::create_directories(dir, ec);

    auto doc = Document::createBlank(Size{8, 8});
    SaveError err = SaveError::None;

    PE_CHECK(!saveDocument(*doc, (dir / "x.qqq").string(), &err));
    PE_CHECK(err == SaveError::UnsupportedFormat);

    // A directory that does not exist cannot receive the temp file.
    err = SaveError::None;
    PE_CHECK(!saveDocument(*doc, (dir / "nope" / "a.pedoc").string(), &err));
    PE_CHECK(err == SaveError::CannotCreate);

    // Over the composite cap the flattening formats fail with the flatten limit rather than
    // anything about the disk. JPEG is one such (TurboJPEG is one-shot); this holds whether or not
    // the JPEG codec is built, since an over-cap flattening format cannot produce bytes either way.
    auto big = Document::createBlank(Size{9000, 9000});  // 81 MP, over the 64 MP cap
    PE_CHECK(big != nullptr);
    err = SaveError::None;
    PE_CHECK(!saveDocument(*big, (dir / "big.jpg").string(), &err));
    PE_CHECK(err == SaveError::TooLargeToFlatten);
    // PNG streams band by band (#165), so the same over-cap document saves rather than refusing.
#ifdef PHOTOEDIT_HAVE_PNG
    err = SaveError::UnsupportedFormat;  // deliberately dirty
    PE_CHECK(saveDocument(*big, (dir / "big.png").string(), &err));
    PE_CHECK(err == SaveError::None);
#endif
    err = SaveError::UnsupportedFormat;  // deliberately dirty
    PE_CHECK(saveDocument(*big, (dir / "big.pedoc").string(), &err));
    PE_CHECK(err == SaveError::None);

    // A document too wide for WebP's format, but comfortably UNDER the composite cap, so
    // nothing about memory explains it. This used to report CodecUnavailable, telling the
    // user their build lacked a codec that is in fact compiled in.
    auto wide = Document::createBlank(Size{kMaxWebpDimension + 1, 100});
    PE_CHECK(wide != nullptr);
    if (wide != nullptr) {
        const std::int64_t wideArea =
            static_cast<std::int64_t>(wide->canvasSize().width) * wide->canvasSize().height;
        PE_CHECK(wideArea < kMaxCompositeImagePixels);  // not a flatten-limit case
        err = SaveError::None;
        PE_CHECK(!saveDocument(*wide, (dir / "wide.webp").string(), &err));
        PE_CHECK(err == SaveError::ExceedsFormatLimit);
#ifdef PHOTOEDIT_HAVE_PNG
        // The same document as PNG is refused by nothing: PNG has no such side limit. Guarded
        // because a build without libpng refuses it for an unrelated reason.
        err = SaveError::UnsupportedFormat;
        PE_CHECK(saveDocument(*wide, (dir / "wide.png").string(), &err));
        PE_CHECK(err == SaveError::None);
#endif
    }

    // Content past the coordinate range the native format stores. The writer refuses it
    // rather than producing a file its own reader would not take back, and the reason has
    // to name the real problem: this used to report CodecUnavailable, which told the user
    // their build was missing a codec that is in fact always compiled in.
    auto out = Document::createBlank(Size{64, 64});
    PE_CHECK(out != nullptr);
    auto* pl = dynamic_cast<PixelLayer*>(out->topLevelLayers()[0].get());
    PE_REQUIRE(pl != nullptr);
    pl->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
    pl->tiles().setPixel(kMaxCanvasDimension + 1000, 5, Rgba8{4, 5, 6, 255});
    const std::string outPath = (dir / "out_of_range.pedoc").string();
    err = SaveError::None;
    PE_CHECK(!saveDocument(*out, outPath, &err));
    PE_CHECK(err == SaveError::ContentOutOfRange);
    // Refused before anything was written: no file, not even a partial or temp one.
    PE_CHECK(!fs::exists(outPath));

    fs::remove_all(dir, ec);
}
