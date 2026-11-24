// flyG - graphics module: unified image I/O (format detection by magic, encode/decode, load/save by extension)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

#include <flyg/graphics/codec/bmp.hpp>
#include <flyg/graphics/codec/gif.hpp>
#include <flyg/graphics/codec/hdr.hpp>
#include <flyg/graphics/codec/jpeg.hpp>
#include <flyg/graphics/codec/png.hpp>
#include <flyg/graphics/codec/pnm.hpp>
#include <flyg/graphics/codec/qoi.hpp>
#include <flyg/graphics/codec/tga.hpp>

namespace flyg::graphics {

enum class ImageFormat { unknown, png, jpeg, gif, bmp, tga, qoi, pnm, hdr };

[[nodiscard]] inline const char* format_name(ImageFormat f) noexcept {
    switch (f) {
        case ImageFormat::png: return "png";
        case ImageFormat::jpeg: return "jpeg";
        case ImageFormat::gif: return "gif";
        case ImageFormat::bmp: return "bmp";
        case ImageFormat::tga: return "tga";
        case ImageFormat::qoi: return "qoi";
        case ImageFormat::pnm: return "pnm";
        case ImageFormat::hdr: return "hdr";
        default: return "unknown";
    }
}

/// Identify a format from the leading bytes (TGA has no magic and is only recognised by a plausible header).
[[nodiscard]] inline ImageFormat detect_format(std::span<const std::byte> in) noexcept {
    const auto* d = reinterpret_cast<const u8*>(in.data());
    const usize n = in.size();
    auto is = [&](const char* s, usize len) { return n >= len && std::memcmp(d, s, len) == 0; };
    if (is("\x89PNG\r\n\x1a\n", 8)) return ImageFormat::png;
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return ImageFormat::jpeg;
    if (is("GIF87a", 6) || is("GIF89a", 6)) return ImageFormat::gif;
    if (is("BM", 2)) return ImageFormat::bmp;
    if (is("qoif", 4)) return ImageFormat::qoi;
    if (is("#?RADIANCE", 10) || is("#?RGBE", 6)) return ImageFormat::hdr;
    if (n >= 3 && d[0] == 'P' && d[1] >= '1' && d[1] <= '7' && (d[2] == '\n' || d[2] == ' ' || d[2] == '\t' || d[2] == '\r' || d[2] == '#')) return ImageFormat::pnm;
    if (n >= 18) {
        const u8 type = d[2], bpp = d[16];
        const bool types_ok = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
        const bool bpp_ok = bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32;
        if (types_ok && bpp_ok && d[1] <= 1 && (d[12] | d[13]) + (d[14] | d[15]) > 0) return ImageFormat::tga;
    }
    return ImageFormat::unknown;
}

[[nodiscard]] inline ImageFormat format_from_extension(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (e == ".png") return ImageFormat::png;
    if (e == ".jpg" || e == ".jpeg" || e == ".jpe" || e == ".jfif") return ImageFormat::jpeg;
    if (e == ".gif") return ImageFormat::gif;
    if (e == ".bmp" || e == ".dib") return ImageFormat::bmp;
    if (e == ".tga" || e == ".icb" || e == ".vda") return ImageFormat::tga;
    if (e == ".qoi") return ImageFormat::qoi;
    if (e == ".ppm" || e == ".pgm" || e == ".pbm" || e == ".pnm" || e == ".pam") return ImageFormat::pnm;
    if (e == ".hdr" || e == ".pic") return ImageFormat::hdr;
    return ImageFormat::unknown;
}

/// Decode any supported format. HDR data is clamped and sRGB-encoded.
[[nodiscard]] inline Image decode_image(std::span<const std::byte> in, ImageFormat* detected = nullptr) {
    const ImageFormat f = detect_format(in);
    if (detected) *detected = f;
    switch (f) {
        case ImageFormat::png: return png::decode(in);
        case ImageFormat::jpeg: return jpeg::decode(in);
        case ImageFormat::gif: return gif::decode(in);
        case ImageFormat::bmp: return bmp::decode(in);
        case ImageFormat::tga: return tga::decode(in);
        case ImageFormat::qoi: return qoi::decode(in);
        case ImageFormat::pnm: return pnm::decode(in);
        case ImageFormat::hdr: {
            const ImageF h = hdr::decode(in);
            Image out(h.width(), h.height());
            for (usize i = 0; i < h.pixel_count(); ++i) out.data()[i] = color::from_linear(h.data()[i]);
            return out;
        }
        default: throw DataException("image: unrecognised file format");
    }
}

struct SaveOptions {
    int jpeg_quality = 90;
    jpeg::Subsampling jpeg_subsampling = jpeg::Subsampling::s420;
    int png_level = 6;
};

[[nodiscard]] inline std::vector<std::byte> encode_image(const Image& img, ImageFormat f, const SaveOptions& o = {}) {
    switch (f) {
        case ImageFormat::png: { png::EncodeOptions po; po.level = o.png_level; return png::encode(img, po); }
        case ImageFormat::jpeg: { jpeg::EncodeOptions jo; jo.quality = o.jpeg_quality; jo.subsampling = o.jpeg_subsampling; return jpeg::encode(img, jo); }
        case ImageFormat::gif: return gif::encode(img);
        case ImageFormat::bmp: return bmp::encode(img);
        case ImageFormat::tga: return tga::encode(img);
        case ImageFormat::qoi: return qoi::encode(img);
        case ImageFormat::pnm: return pnm::encode(img);
        case ImageFormat::hdr: {
            ImageF h(img.width(), img.height());
            for (usize i = 0; i < img.pixel_count(); ++i) h.data()[i] = color::to_linear(img.data()[i]);
            return hdr::encode(h);
        }
        default: throw DataException("image: unsupported output format");
    }
}

[[nodiscard]] inline Image load_image(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw DataException("image: cannot open " + path.string());
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decode_image(std::as_bytes(std::span(raw.data(), raw.size())));
}

/// Format taken from the extension unless given explicitly.
inline void save_image(const Image& img, const std::filesystem::path& path, ImageFormat f = ImageFormat::unknown, const SaveOptions& o = {}) {
    if (f == ImageFormat::unknown) f = format_from_extension(path);
    if (f == ImageFormat::unknown) throw DataException("image: cannot determine the output format of " + path.string());
    const auto bytes = encode_image(img, f, o);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw DataException("image: cannot create " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw DataException("image: write failed for " + path.string());
}

}  // namespace flyg::graphics
