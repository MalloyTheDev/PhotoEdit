#include "pe/core/DocumentIO.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/NativeFormat.hpp"
#include "pe/core/PixelLayer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <system_error>

namespace pe {

ImageFormat formatFromExtension(std::string_view path) {
    // Scope to the file name so a dotted *directory* (e.g. "/dir.png/file") can't be
    // mistaken for an extension.
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return ImageFormat::Unknown;
    std::string ext(name.substr(dot));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".png") return ImageFormat::Png;
    if (ext == ".jpg" || ext == ".jpeg") return ImageFormat::Jpeg;
    if (ext == ".tif" || ext == ".tiff") return ImageFormat::Tiff;
    if (ext == ".webp") return ImageFormat::WebP;
    if (ext == ".psd") return ImageFormat::Psd;
    if (ext == ".pedoc") return ImageFormat::Native;
    return ImageFormat::Unknown;
}

bool formatAvailable(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::Native:
            return true;
        case ImageFormat::Png:
#ifdef PHOTOEDIT_HAVE_PNG
            return true;
#else
            return false;
#endif
        case ImageFormat::Jpeg:
#ifdef PHOTOEDIT_HAVE_JPEG
            return true;
#else
            return false;
#endif
        case ImageFormat::Tiff:
#ifdef PHOTOEDIT_HAVE_TIFF
            return true;
#else
            return false;
#endif
        case ImageFormat::WebP:
#ifdef PHOTOEDIT_HAVE_WEBP
            return true;
#else
            return false;
#endif
        case ImageFormat::Psd:
            return true;  // dependency-free reader; always available (import only)
        case ImageFormat::Unknown:
        default:
            return false;
    }
}

std::unique_ptr<Document> documentFromImage(const PixelBuffer& image) {
    if (image.isEmpty()) return nullptr;
    auto doc = Document::createBlank(Size{image.width(), image.height()});
    if (doc == nullptr) return nullptr;  // size beyond the canvas limits
    auto* layer = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    if (layer == nullptr) return nullptr;  // createBlank always seeds a pixel layer

    TileStore& store = layer->tiles();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) store.setPixel(x, y, image.at(x, y));
    }
    return doc;
}

namespace {
// Decode a raster format to a single-layer document, or nullptr. Always compiled: the PSD
// reader is dependency-free and present in every build, so this is never unused.
std::unique_ptr<Document> fromRaster(std::optional<PixelBuffer>&& image) {
    if (!image) return nullptr;
    return documentFromImage(*image);
}
}  // namespace

std::unique_ptr<Document> importDocument(std::span<const std::byte> data, ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::Native:
            return deserializeDocument(data);
#ifdef PHOTOEDIT_HAVE_PNG
        case ImageFormat::Png:
            return fromRaster(decodePng(data));
#endif
#ifdef PHOTOEDIT_HAVE_JPEG
        case ImageFormat::Jpeg:
            return fromRaster(decodeJpeg(data));
#endif
#ifdef PHOTOEDIT_HAVE_TIFF
        case ImageFormat::Tiff:
            return fromRaster(decodeTiff(data));
#endif
#ifdef PHOTOEDIT_HAVE_WEBP
        case ImageFormat::WebP:
            return fromRaster(decodeWebp(data));
#endif
        case ImageFormat::Psd:
            return fromRaster(decodePsd(data));  // dependency-free; always available
        default:
            return nullptr;  // unknown format or codec not built in
    }
}

std::vector<std::byte> exportDocument(const Document& doc, ImageFormat fmt) {
    return exportDocument(doc, fmt, ExportOptions{});
}

std::vector<std::byte> exportDocument(const Document& doc, ImageFormat fmt,
                                      [[maybe_unused]] const ExportOptions& opts) {
    switch (fmt) {
        case ImageFormat::Native:
            return serializeDocument(doc);
#ifdef PHOTOEDIT_HAVE_PNG
        case ImageFormat::Png:
            return encodePng(doc.compositeImage());
#endif
#ifdef PHOTOEDIT_HAVE_JPEG
        case ImageFormat::Jpeg:
            return encodeJpeg(doc.compositeImage(), opts.jpegQuality);
#endif
#ifdef PHOTOEDIT_HAVE_TIFF
        case ImageFormat::Tiff:
            return encodeTiff(doc.compositeImage());
#endif
#ifdef PHOTOEDIT_HAVE_WEBP
        case ImageFormat::WebP:
            return encodeWebp(doc.compositeImage());
#endif
        default:
            return {};  // unknown format or codec not built in
    }
}

namespace {
// Reading a whole file into memory: cap the size so opening a pathological file can't
// exhaust memory before the decoders' own dimension caps apply.
constexpr std::uintmax_t kMaxFileBytes = 512ull * 1024 * 1024;  // 512 MB
}  // namespace

std::unique_ptr<Document> loadDocument(const std::string& path) {
    const ImageFormat fmt = formatFromExtension(path);
    if (fmt == ImageFormat::Unknown) return nullptr;

    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxFileBytes) return nullptr;
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes.data()), size)) return nullptr;
    return importDocument(bytes, fmt);
}

namespace {

// A temp path beside the destination. Same directory on purpose: a temp in the
// system temp area could sit on a different volume, where the rename is neither
// atomic nor guaranteed to succeed. The suffix is unique enough that a leftover
// temp or a concurrent save does not collide; a collision would only cause a clean
// save failure, never a corrupted destination.
std::filesystem::path tempSiblingPath(const std::filesystem::path& dst) {
    static unsigned long long seq = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path tmp = dst;
    tmp += ".pe-save-" + std::to_string(stamp) + "-" + std::to_string(seq++) + ".tmp";
    return tmp;
}

}  // namespace

bool saveDocument(const Document& doc, const std::string& path) {
    return saveDocument(doc, path, ExportOptions{});
}

bool saveDocument(const Document& doc, const std::string& path, const ExportOptions& opts) {
    const ImageFormat fmt = formatFromExtension(path);
    if (fmt == ImageFormat::Unknown) return false;

    const std::vector<std::byte> bytes = exportDocument(doc, fmt, opts);
    if (bytes.empty()) return false;

    // Crash-safe write, as docs/systems/20-file-io.md requires: the destination is
    // never opened for writing. Everything goes to a sibling temp file which only
    // replaces the original once it is completely written, so a failure at any point
    // below (disk full, an IO error, the process dying) leaves the user's existing
    // file exactly as it was. Previously this opened the destination with
    // std::ios::trunc, so any of those destroyed a file the user had already saved.
    //
    // Scope of the guarantee: this protects against write errors and process death.
    // It does not by itself guarantee durability across a power loss, which would
    // additionally require flushing the temp file's contents to the storage device
    // before the rename; std::ofstream exposes no portable way to do that.
    const std::filesystem::path dst(path);
    const std::filesystem::path tmp = tempSiblingPath(dst);

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;  // could not create the temp; original untouched
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();  // flushes; some errors only surface here
        if (!file) {
            std::error_code rm;
            std::filesystem::remove(tmp, rm);
            return false;
        }
    }

    // Atomic replace. On failure (for example another process holding the
    // destination open) the original survives and the temp is cleaned up, so the
    // save reports failure rather than leaving a corrupted file behind.
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(tmp, rm);
        return false;
    }
    return true;
}

}  // namespace pe
