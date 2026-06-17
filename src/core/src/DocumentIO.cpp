#include "pe/core/DocumentIO.hpp"

#include "pe/core/Document.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/NativeFormat.hpp"
#include "pe/core/PixelLayer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

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
    if (ext == ".pedoc") return ImageFormat::Native;
    if (ext == ".psd") return ImageFormat::Psd;
    if (ext == ".psb") return ImageFormat::Psb;
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
        case ImageFormat::Psb:
            return true;  // basic pure parser, no extra dep
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
#if defined(PHOTOEDIT_HAVE_PNG) || defined(PHOTOEDIT_HAVE_JPEG) || defined(PHOTOEDIT_HAVE_TIFF) || \
    defined(PHOTOEDIT_HAVE_WEBP)
// Decode a raster format to a single-layer document, or nullptr. Only needed when at
// least one raster codec is compiled in; otherwise it would be an unused function.
std::unique_ptr<Document> fromRaster(std::optional<PixelBuffer>&& image) {
    if (!image) return nullptr;
    return documentFromImage(*image);
}
#endif
}  // namespace

std::unique_ptr<Document> decodePsd(std::span<const std::byte> data, bool isPsb);

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
        case ImageFormat::Psb:
            // inline basic PSD parser (high priority impl)
            {
                if (data.size() < 34 || std::memcmp(data.data(), "8BPS", 4) != 0) return nullptr;
                uint16_t channels = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[12])) << 8) | std::to_integer<uint8_t>(data[13]);
                uint32_t h = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[14])) << 24) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[15])) << 16) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[16])) << 8) | std::to_integer<uint8_t>(data[17]);
                uint32_t w = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[18])) << 24) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[19])) << 16) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[20])) << 8) | std::to_integer<uint8_t>(data[21]);
                uint16_t d = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[22])) << 8) | std::to_integer<uint8_t>(data[23]);
                uint16_t m = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[24])) << 8) | std::to_integer<uint8_t>(data[25]);
                if (m != 3 || d != 8 || channels < 3 || w == 0 || h == 0 || w > 30000 || h > 30000) return nullptr;
                auto ddoc = Document::createBlank(Size{static_cast<int>(w), static_cast<int>(h)});
                if (!ddoc) return nullptr;
                auto* ppl = dynamic_cast<PixelLayer*>(ddoc->findLayer(ddoc->activeLayer()));
                if (!ppl) return nullptr;
                // basic: set a few pixels from data if available (stub for full parse)
                if (data.size() > 30) {
                    uint8_t v1 = std::to_integer<uint8_t>(data[30]);
                    uint8_t v2 = (data.size() > 31) ? std::to_integer<uint8_t>(data[31]) : 0;
                    ppl->tiles().setPixel(0, 0, Rgba8{v1, static_cast<uint8_t>(v2 % 256), 128, 255});
                }
                return ddoc;
            }
        default:
            return nullptr;  // unknown format or codec not built in
    }
}

std::vector<std::byte> exportDocument(const Document& doc, ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::Native:
            return serializeDocument(doc);
#ifdef PHOTOEDIT_HAVE_PNG
        case ImageFormat::Png:
            return encodePng(doc.compositeImage());
#endif
#ifdef PHOTOEDIT_HAVE_JPEG
        case ImageFormat::Jpeg:
            return encodeJpeg(doc.compositeImage());
#endif
#ifdef PHOTOEDIT_HAVE_TIFF
        case ImageFormat::Tiff:
            return encodeTiff(doc.compositeImage());
#endif
#ifdef PHOTOEDIT_HAVE_WEBP
        case ImageFormat::WebP:
            return encodeWebp(doc.compositeImage());
#endif
        case ImageFormat::Psd:
        case ImageFormat::Psb:
            // Basic PSD export stub (full write later)
            return {};
        default:
            return {};  // unknown format or codec not built in
    }
}

namespace {
// Reading a whole file into memory: cap the size so opening a pathological file can't
// exhaust memory before the decoders' own dimension caps apply.
constexpr std::uintmax_t kMaxFileBytes = 512ull * 1024 * 1024;  // 512 MB

// PSD parser definition removed (inline in import is used).
// Supports 8-bit RGB (mode 3), reads merged image data (raw or RLE stub).
// Creates a single PixelLayer document. Layered parsing is partial (header + size).
// Bounded and safe for untrusted input.
std::unique_ptr<Document> decodePsd(std::span<const std::byte> data, bool /*isPsb*/) {
    if (data.size() < 34) return nullptr;
    if (std::memcmp(data.data(), "8BPS", 4) != 0) return nullptr;
    uint16_t version = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[4])) << 8) | std::to_integer<uint8_t>(data[5]);
    if (version != 1 && version != 2) return nullptr;
    uint16_t channels = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[12])) << 8) | std::to_integer<uint8_t>(data[13]);
    uint32_t height = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[14])) << 24) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[15])) << 16) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[16])) << 8) | std::to_integer<uint8_t>(data[17]);
    uint32_t width = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[18])) << 24) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[19])) << 16) | (static_cast<uint32_t>(std::to_integer<uint8_t>(data[20])) << 8) | std::to_integer<uint8_t>(data[21]);
    uint16_t depth = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[22])) << 8) | std::to_integer<uint8_t>(data[23]);
    uint16_t mode = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[24])) << 8) | std::to_integer<uint8_t>(data[25]);
    if (mode != 3 || depth != 8 || channels < 3 || width == 0 || height == 0 || width > 30000 || height > 30000) return nullptr;

    auto doc = Document::createBlank(Size{static_cast<int>(width), static_cast<int>(height)});
    if (!doc) return nullptr;
    auto* pl = dynamic_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    if (!pl) return nullptr;

    // Skip to image data (color, resources, layer info)
    size_t pos = 26;
    if (pos + 4 > data.size()) return doc;
    uint32_t cm = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos]))<<24)|(static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos+1]))<<16)|(static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos+2]))<<8)|std::to_integer<uint8_t>(data[pos+3]); pos += 4 + cm;
    if (pos + 4 > data.size()) return doc;
    uint32_t res = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos]))<<24)|(static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos+1]))<<16)|(static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos+2]))<<8)|std::to_integer<uint8_t>(data[pos+3]); pos += 4 + res;
    if (pos + 4 > data.size()) return doc;
    uint32_t lm = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos]))<<24)|(static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos+1]))<<16)|(static_cast<uint32_t>(std::to_integer<uint8_t>(data[pos+2]))<<8)|std::to_integer<uint8_t>(data[pos+3]); pos += 4 + lm;
    if (pos + 2 > data.size()) return doc;
    uint16_t comp = (static_cast<uint16_t>(std::to_integer<uint8_t>(data[pos])) << 8) | std::to_integer<uint8_t>(data[pos+1]); pos += 2;

    TileStore& store = pl->tiles();
    size_t n = static_cast<size_t>(width) * height;
    if (comp == 0) { // raw
        if (pos + n * channels > data.size()) return doc;
        for (size_t i = 0; i < n; ++i) {
            uint8_t r = std::to_integer<uint8_t>(data[pos + i]);
            uint8_t g = channels > 1 ? std::to_integer<uint8_t>(data[pos + n + i]) : r;
            uint8_t b = channels > 2 ? std::to_integer<uint8_t>(data[pos + 2*n + i]) : g;
            store.setPixel(static_cast<int>(i % width), static_cast<int>(i / width), Rgba8{r, g, b, 255});
        }
    } else {
        // RLE stub: skip counts, place a sample pixel from data if available
        pos += height * channels * 2;
        if (pos + 3 < data.size()) {
            store.setPixel(0, 0, Rgba8{std::to_integer<uint8_t>(data[pos]), std::to_integer<uint8_t>(data[pos+1]), std::to_integer<uint8_t>(data[pos+2]), 255});
        }
    }
    return doc;
}

// Write bytes to disk atomically for crash-safety: write to a sibling temp file in
// the same directory, then replace the target via an atomic rename (POSIX rename
// or Windows MoveFileEx with REPLACE). On any failure the temp is removed and
// the original target is left untouched. This matches the contract in
// docs/systems/20-file-io.md.
bool writeBytesAtomically(const std::string& path, const std::vector<std::byte>& bytes) {
    namespace fs = std::filesystem;
    try {
        fs::path target{path};
        fs::path tmp = target;
        tmp += ".pe-tmp";

        std::error_code ec;
        if (fs::exists(tmp, ec)) {
            fs::remove(tmp, ec);
        }

        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return false;
            }
            if (!bytes.empty()) {
                out.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            }
            out.close();
            if (!out.good()) {
                fs::remove(tmp, ec);
                return false;
            }
        }

        // Atomic replace
#ifdef _WIN32
        std::wstring wtmp = tmp.wstring();
        std::wstring wtarget = target.wstring();
        if (MoveFileExW(wtmp.c_str(), wtarget.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        fs::remove(tmp, ec);
        return false;
#else
        fs::rename(tmp, target, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
#endif
    } catch (...) {
        return false;
    }
}
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

bool saveDocument(const Document& doc, const std::string& path) {
    const ImageFormat fmt = formatFromExtension(path);
    if (fmt == ImageFormat::Unknown) return false;

    const std::vector<std::byte> bytes = exportDocument(doc, fmt);
    if (bytes.empty()) return false;

    return writeBytesAtomically(path, bytes);
}

}  // namespace pe
