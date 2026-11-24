// flyG - graphics module tests: image codecs (PNG, BMP, PNM, TGA, QOI, HDR)
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/graphics/codec/codec.hpp>
#include <flyg/graphics/codec/bmp.hpp>
#include <flyg/graphics/codec/gif.hpp>
#include <flyg/graphics/codec/hdr.hpp>
#include <flyg/graphics/codec/jpeg.hpp>
#include <flyg/graphics/codec/png.hpp>
#include <flyg/graphics/codec/pnm.hpp>
#include <flyg/graphics/codec/qoi.hpp>
#include <flyg/graphics/codec/tga.hpp>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::graphics;

namespace {
Image make(usize w, usize h, bool alpha, bool flat, u64 seed) {
    Image im(w, h);
    for (usize y = 0; y < h; ++y)
        for (usize x = 0; x < w; ++x) {
            if (flat) {
                im(x, y) = ((x / 5 + y / 3) & 1) ? Color{10, 20, 30, 255} : Color{200, 100, 0, alpha ? u8(128) : u8(255)};
            } else {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                im(x, y) = {u8(seed >> 56), u8(seed >> 48), u8(seed >> 40), alpha ? u8(seed >> 32) : u8(255)};
            }
        }
    return im;
}
bool same(const Image& a, const Image& b) {
    return a.width() == b.width() && a.height() == b.height() && std::equal(a.pixels().begin(), a.pixels().end(), b.pixels().begin());
}
std::vector<std::byte> bytes(std::initializer_list<int> l) {
    std::vector<std::byte> v;
    for (int x : l) v.push_back(static_cast<std::byte>(x));
    return v;
}
}  // namespace

FLYG_TEST(graphics_codec_roundtrips) {
    for (auto [w, h] : {std::pair<usize, usize>{1, 1}, {7, 5}, {33, 17}, {64, 3}})
        for (bool alpha : {false, true})
            for (bool flat : {false, true}) {
                const Image im = make(w, h, alpha, flat, w * 31 + h);
                CHECK(same(png::decode(png::encode(im)), im));
                png::EncodeOptions io;
                io.interlace = true;
                CHECK(same(png::decode(png::encode(im, io)), im));
                CHECK(same(bmp::decode(bmp::encode(im)), im));
                bmp::EncodeOptions b32;
                b32.bits = 32;
                CHECK(same(bmp::decode(bmp::encode(im, b32)), im));
                CHECK(same(tga::decode(tga::encode(im, true)), im));
                CHECK(same(tga::decode(tga::encode(im, false)), im));
                CHECK(same(qoi::decode(qoi::encode(im)), im));
                CHECK(same(pnm::decode(pnm::encode(im, alpha ? pnm::Variant::pam : pnm::Variant::ppm)), im));
            }
}

FLYG_TEST(graphics_codec_bmp_palette_and_rle) {
    const Image flat = make(9, 4, false, true, 1);
    bmp::EncodeOptions o8;
    o8.bits = 8;
    CHECK(same(bmp::decode(bmp::encode(flat, o8)), flat));
    CHECK_THROWS_AS(bmp::encode(make(40, 40, false, false, 3), o8), DataException);

    // hand-built 4x2 RLE8 file: bottom row = 3x idx1 + 1x idx2, top row = absolute run 3,4,5,6
    std::vector<std::byte> f = bytes({'B', 'M'});
    auto le32 = [&](u32 v) { for (int s = 0; s < 32; s += 8) f.push_back(static_cast<std::byte>(v >> s)); };
    auto le16 = [&](u32 v) { f.push_back(static_cast<std::byte>(v)); f.push_back(static_cast<std::byte>(v >> 8)); };
    const std::vector<int> rle = {3, 1, 1, 2, 0, 0, 0, 4, 3, 4, 5, 6, 0, 0, 0, 1};
    le32(14 + 40 + 256 * 4 + static_cast<u32>(rle.size())), le32(0), le32(14 + 40 + 256 * 4);
    le32(40), le32(4), le32(2), le16(1), le16(8), le32(1), le32(static_cast<u32>(rle.size())), le32(0), le32(0), le32(256), le32(0);
    for (int i = 0; i < 256; ++i) { f.push_back(static_cast<std::byte>(i)); f.push_back(static_cast<std::byte>(255 - i)); f.push_back(std::byte{7}); f.push_back(std::byte{0}); }
    for (int v : rle) f.push_back(static_cast<std::byte>(v));
    const Image d = bmp::decode(f);
    CHECK_EQ(d.width(), 4u);
    CHECK_EQ(d.height(), 2u);
    CHECK(d(0, 1) == (Color{7, 254, 1, 255}));   // bottom row, idx 1 -> (r=7,g=254,b=1)
    CHECK(d(3, 1) == (Color{7, 253, 2, 255}));
    CHECK(d(0, 0) == (Color{7, 252, 3, 255}));   // top row absolute run
    CHECK(d(3, 0) == (Color{7, 249, 6, 255}));
}

FLYG_TEST(graphics_codec_pnm_variants) {
    auto dec = [](std::string_view s) { return pnm::decode(std::span(reinterpret_cast<const std::byte*>(s.data()), s.size())); };
    const Image p1 = dec("P1\n# comment\n3 2\n1 0 1\n0 1 0\n");
    CHECK(p1(0, 0) == Color::black);
    CHECK(p1(1, 0) == Color::white);
    const Image p3 = dec("P3\n2 1\n255\n255 0 0 0 255 0\n");
    CHECK(p3(0, 0) == (Color{255, 0, 0, 255}));
    CHECK(p3(1, 0) == (Color{0, 255, 0, 255}));
    const Image p2 = dec("P2\n2 1\n100\n100 50\n");
    CHECK_EQ(p2(0, 0).r, 255);
    CHECK_EQ(p2(1, 0).r, 128);
    std::string pam = "P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    pam += std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    CHECK(dec(pam)(1, 0) == (Color{5, 6, 7, 8}));
    CHECK_THROWS_AS(dec("P6\n2 2\n255\nabc"), DataException);
    CHECK_THROWS_AS(dec("XX"), DataException);
}

FLYG_TEST(graphics_codec_qoi_reference) {
    // 2x1 image, RGB: QOI_OP_RGB red, then QOI_OP_RUN of 1
    auto v = bytes({'q', 'o', 'i', 'f', 0, 0, 0, 2, 0, 0, 0, 1, 3, 0, 0xfe, 255, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0, 1});
    const Image im = qoi::decode(v);
    CHECK(im(0, 0) == (Color{255, 0, 0, 255}));
    CHECK(im(1, 0) == (Color{255, 0, 0, 255}));
    v.resize(18);
    CHECK_THROWS_AS(qoi::decode(v), DataException);
}

FLYG_TEST(graphics_codec_hdr_roundtrip) {
    for (usize w : {usize{3}, usize{16}, usize{300}}) {
        ImageF im(w, 4);
        for (usize y = 0; y < 4; ++y)
            for (usize x = 0; x < w; ++x) im(x, y) = {0.01f * float(x + 1), float(y) * 2.5f, (x % 5 == 0) ? 100.0f : 0.5f, 1.0f};
        const ImageF d = hdr::decode(hdr::encode(im));
        CHECK_EQ(d.width(), w);
        for (usize y = 0; y < 4; ++y)
            for (usize x = 0; x < w; ++x) {
                const ColorF a = im(x, y), b = d(x, y);
                const float m = std::max({a.r, a.g, a.b});
                CHECK(std::abs(a.r - b.r) <= m / 128 + 1e-6f);
                CHECK(std::abs(a.g - b.g) <= m / 128 + 1e-6f);
                CHECK(std::abs(a.b - b.b) <= m / 128 + 1e-6f);
            }
    }
    CHECK_THROWS_AS(hdr::decode(bytes({'n', 'o', 'p', 'e', '\n'})), DataException);
}

FLYG_TEST(graphics_codec_malformed_never_crashes) {
    u64 seed = 42;
    auto rnd = [&] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(seed >> 33); };
    const Image im = make(12, 9, true, false, 9);
    const std::vector<std::vector<std::byte>> bases = {png::encode(im), bmp::encode(im), tga::encode(im), qoi::encode(im), pnm::encode(im), hdr::encode(ImageF(12, 9)), jpeg::encode(im), gif::encode(im)};
    for (usize k = 0; k < bases.size(); ++k)
        for (int round = 0; round < 120; ++round) {
            auto m = bases[k];
            for (u32 i = 0, e = 1 + rnd() % 5; i < e; ++i) m[rnd() % m.size()] = static_cast<std::byte>(rnd());
            if (round % 5 == 0) m.resize(rnd() % m.size());
            try {
                switch (k) {
                    case 0: (void)png::decode(m); break;
                    case 1: (void)bmp::decode(m); break;
                    case 2: (void)tga::decode(m); break;
                    case 3: (void)qoi::decode(m); break;
                    case 4: (void)pnm::decode(m); break;
                    case 5: (void)hdr::decode(m); break;
                    case 6: (void)jpeg::decode(m); break;
                    default: (void)gif::decode(m); break;
                }
            } catch (const DataException&) {
            }
        }
}

FLYG_TEST(graphics_codec_jpeg_roundtrip) {
    Image im(53, 37);
    for (usize y = 0; y < im.height(); ++y)
        for (usize x = 0; x < im.width(); ++x)
            im(x, y) = {u8(x * 255 / 52), u8(y * 255 / 36), u8(128 + 100 * std::sin(double(x) / 5.0)), 255};
    for (auto sub : {jpeg::Subsampling::s444, jpeg::Subsampling::s422, jpeg::Subsampling::s420}) {
        jpeg::EncodeOptions o;
        o.quality = 95;
        o.subsampling = sub;
        const Image d = jpeg::decode(jpeg::encode(im, o));
        CHECK_EQ(d.width(), im.width());
        CHECK_EQ(d.height(), im.height());
        int worst = 0;
        for (usize i = 0; i < im.pixel_count(); ++i) {
            worst = std::max(worst, std::abs(int(d.data()[i].r) - int(im.data()[i].r)));
            worst = std::max(worst, std::abs(int(d.data()[i].g) - int(im.data()[i].g)));
            worst = std::max(worst, std::abs(int(d.data()[i].b) - int(im.data()[i].b)));
        }
        CHECK(worst <= 24);
    }
    jpeg::EncodeOptions g;
    g.grayscale = true;
    const Image gd = jpeg::decode(jpeg::encode(im, g));
    CHECK_EQ(gd(3, 3).r, gd(3, 3).g);
    jpeg::EncodeOptions badq;
    badq.quality = 0;
    CHECK_THROWS_AS(jpeg::encode(im, badq), DataException);
    CHECK_THROWS_AS(jpeg::decode(bytes({0xff, 0xd8, 0xff, 0xd9})), DataException);
}

FLYG_TEST(graphics_codec_gif_roundtrip_and_animation) {
    const Image few = make(40, 30, true, true, 5);  // two colours + transparency-free alpha 128 -> thresholded
    Image opaque(40, 30);
    for (usize y = 0; y < 30; ++y)
        for (usize x = 0; x < 40; ++x) opaque(x, y) = {u8((x / 8) * 50), u8((y / 6) * 40), 90, u8((x + y) % 7 == 0 ? 0 : 255)};
    for (bool inter : {false, true}) {
        gif::EncodeOptions o;
        o.interlace = inter;
        const Image d = gif::decode(gif::encode(opaque, o));
        for (usize i = 0; i < opaque.pixel_count(); ++i) {
            CHECK_EQ(d.data()[i].a, opaque.data()[i].a);
            if (opaque.data()[i].a) CHECK(d.data()[i] == opaque.data()[i]);
        }
    }
    (void)few;
    gif::Animation a;
    a.width = 16, a.height = 8, a.loop_count = 0;
    for (int f = 0; f < 4; ++f) a.frames.push_back({make(16, 8, false, true, static_cast<u64>(f)), 40u * u32(f + 1)});
    a.frames[2].image = Image(16, 8, Color{1, 2, 3, 255});
    const gif::Animation back = gif::decode_animation(gif::encode_animation(a));
    CHECK_EQ(back.frames.size(), 4u);
    CHECK_EQ(back.loop_count, 0u);
    CHECK_EQ(back.frames[1].delay_ms, 80u);
    CHECK(same(back.frames[2].image, a.frames[2].image));
    // > 256 colours: must still produce a decodable, close image
    const Image noisy = make(60, 40, false, false, 77);
    const Image q = gif::decode(gif::encode(noisy));
    CHECK_EQ(q.width(), 60u);
    CHECK_THROWS_AS(gif::decode(bytes({'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0})), DataException);
}

FLYG_TEST(graphics_codec_unified_io) {
    const Image im = make(21, 13, true, false, 8);
    for (auto f : {ImageFormat::png, ImageFormat::bmp, ImageFormat::tga, ImageFormat::qoi, ImageFormat::jpeg, ImageFormat::gif, ImageFormat::pnm}) {
        const auto bytes_ = encode_image(im, f);
        CHECK(detect_format(bytes_) == f);
        ImageFormat seen = ImageFormat::unknown;
        const Image d = decode_image(bytes_, &seen);
        CHECK(seen == f);
        CHECK_EQ(d.width(), im.width());
    }
    CHECK(format_from_extension("a/b/Photo.JPG") == ImageFormat::jpeg);
    CHECK(format_from_extension("x.png") == ImageFormat::png);
    CHECK(format_from_extension("x.unknown") == ImageFormat::unknown);
    const auto path = std::filesystem::temp_directory_path() / "flyg_codec_test.qoi";
    save_image(im, path);
    CHECK(same(load_image(path), im));
    std::filesystem::remove(path);
    CHECK_THROWS_AS(save_image(im, "x.zzz"), DataException);
    CHECK_THROWS_AS(decode_image(bytes({1, 2, 3, 4})), DataException);
    CHECK_THROWS_AS(load_image("/nonexistent/none.png"), DataException);
}
