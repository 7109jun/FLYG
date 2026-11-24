// flyG - graphics module tests: 2D canvas (paths, stroking, fill rules, clipping, paints, blending, fonts)
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/graphics/graphics.hpp>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::graphics;

namespace {
double alpha_sum(const Image& im) {
    double s = 0;
    for (const Color& c : im.pixels()) s += c.a / 255.0;
    return s;
}
Canvas blank(usize w = 100, usize h = 100) { return Canvas(w, h, Color{0, 0, 0, 0}); }

// ---- minimal TrueType builder -------------------------------------------------------------------------------------
using Buf = std::vector<u8>;
void be16(Buf& b, u32 v) { b.push_back(static_cast<u8>(v >> 8)); b.push_back(static_cast<u8>(v)); }
void be32(Buf& b, u32 v) { be16(b, v >> 16); be16(b, v & 0xffff); }

std::vector<std::byte> make_font(bool with_kern) {
    // glyph 0: unit square, glyph 1: quadratic arch, glyph 2: composite (arch scaled 0.5 and moved by 100,200)
    Buf g0, g1, g2;
    be16(g0, 1), be16(g0, 0), be16(g0, 0), be16(g0, 1000), be16(g0, 1000);       // 1 contour, bbox
    be16(g0, 3), be16(g0, 0);                                                      // endPts, instructionLength
    for (int f : {1, 1, 1, 1}) g0.push_back(static_cast<u8>(f));
    for (int dx : {0, 1000, 0, -1000}) be16(g0, static_cast<u32>(dx) & 0xffff);   // x deltas
    for (int dy : {0, 0, 1000, 0}) be16(g0, static_cast<u32>(dy) & 0xffff);       // y deltas
    be16(g1, 1), be16(g1, 0), be16(g1, 0), be16(g1, 1000), be16(g1, 1000);
    be16(g1, 2), be16(g1, 0);
    for (int f : {1, 0, 1}) g1.push_back(static_cast<u8>(f));
    for (int dx : {0, 500, 500}) be16(g1, static_cast<u32>(dx) & 0xffff);
    for (int dy : {0, 1000, -1000}) be16(g1, static_cast<u32>(dy) & 0xffff);
    be16(g2, 0xffff), be16(g2, 100), be16(g2, 200), be16(g2, 600), be16(g2, 700);
    be16(g2, 0x000b), be16(g2, 1), be16(g2, 100), be16(g2, 200), be16(g2, 0x2000);
    Buf glyf, loca;
    for (Buf* g : {&g0, &g1, &g2}) {
        be32(loca, static_cast<u32>(glyf.size()));
        glyf.insert(glyf.end(), g->begin(), g->end());
        while (glyf.size() % 4) glyf.push_back(0);
    }
    be32(loca, static_cast<u32>(glyf.size()));
    Buf head(54, 0);
    head[18] = 0x03, head[19] = 0xe8;  // unitsPerEm 1000
    head[51] = 1;                      // long loca
    Buf maxp;
    be32(maxp, 0x00010000), be16(maxp, 3);
    Buf hhea(36, 0);
    hhea[4] = 0x03, hhea[5] = 0x20;                                   // ascent 800
    hhea[6] = 0xff, hhea[7] = 0x38;                                   // descent -200
    hhea[35] = 3;                                                     // numberOfHMetrics
    Buf hmtx;
    for (int adv : {600, 700, 800}) { be16(hmtx, static_cast<u32>(adv)); be16(hmtx, 0); }
    Buf cmap;
    be16(cmap, 0), be16(cmap, 1), be16(cmap, 3), be16(cmap, 1), be32(cmap, 12);
    // format 4: two segments: 'A'..'B' -> glyph 1.., and the terminator
    be16(cmap, 4), be16(cmap, 32), be16(cmap, 0), be16(cmap, 4), be16(cmap, 4), be16(cmap, 1), be16(cmap, 0);
    be16(cmap, 66), be16(cmap, 0xffff), be16(cmap, 0);                 // endCode, pad
    be16(cmap, 65), be16(cmap, 0xffff);                                // startCode
    be16(cmap, static_cast<u32>(1 - 65) & 0xffff), be16(cmap, 1);      // idDelta
    be16(cmap, 0), be16(cmap, 0);                                      // idRangeOffset
    Buf kern;
    if (with_kern) {
        be16(kern, 0), be16(kern, 1);
        be16(kern, 0), be16(kern, 20), be16(kern, 1);                  // version, length, coverage (horizontal, format 0)
        be16(kern, 1), be16(kern, 6), be16(kern, 0), be16(kern, 0);    // nPairs, searchRange..
        be16(kern, 1), be16(kern, 2), be16(kern, static_cast<u32>(-80) & 0xffff);
    }
    struct T { const char* tag; const Buf* d; };
    std::vector<T> tabs = {{"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea}, {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}};
    if (with_kern) tabs.push_back({"kern", &kern});
    Buf out;
    be32(out, 0x00010000), be16(out, static_cast<u32>(tabs.size())), be16(out, 0), be16(out, 0), be16(out, 0);
    usize off = 12 + tabs.size() * 16;
    for (auto& t : tabs) {
        out.insert(out.end(), t.tag, t.tag + 4);
        be32(out, 0), be32(out, static_cast<u32>(off)), be32(out, static_cast<u32>(t.d->size()));
        off += (t.d->size() + 3) & ~usize(3);
    }
    for (auto& t : tabs) {
        out.insert(out.end(), t.d->begin(), t.d->end());
        while (out.size() % 4) out.push_back(0);
    }
    std::vector<std::byte> r(out.size());
    std::memcpy(r.data(), out.data(), out.size());
    return r;
}
double signed_area(const Path& p) {
    double area = 0;
    for (const auto& pl : p.flatten(0.02f))
        for (usize k = 0; k < pl.pts.size(); ++k) {
            const Point a = pl.pts[k], b = pl.pts[(k + 1) % pl.pts.size()];
            area += (static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y) / 2;
        }
    return area;
}
}  // namespace

FLYG_TEST(graphics_canvas_exact_coverage) {
    Canvas c = blank(10, 10);
    c.set_fill(Color{255, 0, 0});
    c.fill_rect(2.25f, 3.0f, 3.5f, 2.0f);  // x: 2.25..5.75, y: 3..5
    CHECK_EQ(c.pixel(3, 3).a, 255);
    CHECK_EQ(c.pixel(2, 3).a, 191);        // 75% coverage
    CHECK_EQ(c.pixel(5, 4).a, 191);
    CHECK_EQ(c.pixel(6, 3).a, 0);
    CHECK_EQ(c.pixel(3, 5).a, 0);
    CHECK(std::fabs(alpha_sum(c.image()) - 7.0) < 0.05);

    Canvas d = blank(80, 80);
    d.set_fill(Color::black);
    d.fill_circle(40, 40, 30);
    CHECK(std::fabs(alpha_sum(d.image()) - 3.14159265 * 900) < 0.01 * 3.14159265 * 900);
}

FLYG_TEST(graphics_canvas_fill_rules_and_clip) {
    Path p;
    p.rect(10, 10, 80, 80).rect(30, 30, 40, 40);  // same winding: nonzero fills the hole, even-odd does not
    Canvas a = blank(), b = blank();
    a.set_fill(Color::black), b.set_fill(Color::black);
    a.fill(p, FillRule::non_zero);
    b.fill(p, FillRule::even_odd);
    CHECK_EQ(a.pixel(50, 50).a, 255);
    CHECK_EQ(b.pixel(50, 50).a, 0);
    CHECK_EQ(b.pixel(15, 15).a, 255);

    Canvas c = blank();
    c.save();
    c.clip(Path().circle(50, 50, 20));
    c.set_fill(Color{0, 255, 0});
    c.fill_rect(0, 0, 100, 100);
    c.restore();
    CHECK_EQ(c.pixel(50, 50).g, 255);
    CHECK_EQ(c.pixel(5, 5).a, 0);
    c.fill_rect(0, 0, 10, 10);  // clip removed by restore
    CHECK_EQ(c.pixel(5, 5).a, 255);
    CHECK_THROWS_AS(c.restore(), GraphicsException);
}

FLYG_TEST(graphics_canvas_stroking) {
    Canvas c = blank(120, 60);
    c.set_stroke(Color::black);
    c.set_line_width(4);
    c.draw_line(10, 30, 110, 30);
    CHECK(std::fabs(alpha_sum(c.image()) - 400.0) < 2.0);
    Canvas sq = blank(120, 60);
    sq.set_stroke(Color::black), sq.set_line_width(10), sq.set_line_cap(LineCap::square);
    sq.draw_line(20, 30, 100, 30);
    CHECK(std::fabs(alpha_sum(sq.image()) - 900.0) < 3.0);  // (80 + 10) x 10
    Canvas rd = blank(120, 60);
    rd.set_stroke(Color::black), rd.set_line_width(10), rd.set_line_cap(LineCap::round);
    rd.draw_line(20, 30, 100, 30);
    CHECK(std::fabs(alpha_sum(rd.image()) - (800.0 + 3.14159265 * 25)) < 4.0);

    Canvas dash = blank(120, 20);
    dash.set_stroke(Color::black), dash.set_line_width(4), dash.set_line_dash({10, 10});
    dash.draw_line(0, 10, 100, 10);
    CHECK(std::fabs(alpha_sum(dash.image()) - 200.0) < 3.0);

    // closed rectangle outline: area = outer - inner (miter joins)
    Canvas rc = blank(100, 100);
    rc.set_stroke(Color::black), rc.set_line_width(6);
    rc.stroke_rect(20, 20, 60, 40);
    CHECK(std::fabs(alpha_sum(rc.image()) - (66.0 * 46.0 - 54.0 * 34.0)) < 3.0);
    // miter limit: a sharp spike falls back to a bevel
    Canvas sp = blank(100, 100);
    sp.set_stroke(Color::black), sp.set_line_width(6), sp.set_miter_limit(2);
    sp.stroke(Path().move_to(10, 50).line_to(90, 48).line_to(10, 46));
    CHECK_EQ(sp.pixel(97, 48).a, 0);
    CHECK_THROWS_AS(stroke(Path().move_to(0, 0).line_to(1, 1), [] { StrokeStyle z; z.width = 0; return z; }()), GraphicsException);
}

FLYG_TEST(graphics_canvas_transforms) {
    Canvas c = blank(100, 100);
    c.set_fill(Color::black);
    c.translate(50, 50);
    c.rotate(kPi / 4);
    c.fill_rect(-10, -10, 20, 20);  // rotated square, area preserved
    CHECK(std::fabs(alpha_sum(c.image()) - 400.0) < 3.0);
    CHECK_EQ(c.pixel(50, 50).a, 255);
    CHECK_EQ(c.pixel(50, 38).a, 255);  // diamond tip is ~14 px away
    CHECK_EQ(c.pixel(40, 40).a, 0);
    const Transform t = Transform::translate(3, 4) * Transform::scale(2, 2);
    const Point p = t({1, 1});
    CHECK_EQ(p.x, 5.0f);
    CHECK_EQ(p.y, 6.0f);
    const auto inv = t.inverse();
    CHECK(inv.has_value());
    CHECK(std::fabs((*inv)(p).x - 1) < 1e-5f);
    CHECK(!Transform::scale(0, 1).inverse().has_value());
}

FLYG_TEST(graphics_canvas_paints) {
    Canvas c = blank(100, 10);
    c.set_fill(linear_gradient({0, 0}, {100, 0}, Gradient{{0, Color{0, 0, 0}}, {1, Color{255, 255, 255}}}));
    c.fill_rect(0, 0, 100, 10);
    CHECK(c.pixel(0, 5).r <= 3);
    CHECK(c.pixel(99, 5).r >= 252);
    CHECK(std::abs(int(c.pixel(50, 5).r) - 128) <= 3);
    Canvas r = blank(100, 100);
    r.set_fill(radial_gradient({50, 50}, 40, Gradient{{0, Color{255, 0, 0}}, {1, Color{0, 0, 255}}}));
    r.fill_rect(0, 0, 100, 100);
    CHECK(r.pixel(50, 50).r > 245);
    CHECK(r.pixel(0, 0).b == 255);  // pad
    Canvas k = blank(100, 100);
    k.set_fill(conic_gradient({50, 50}, 0, Gradient{{0, Color{255, 0, 0}}, {0.5f, Color{0, 255, 0}}, {1, Color{255, 0, 0}}}));
    k.fill_rect(0, 0, 100, 100);
    CHECK(k.pixel(90, 50).r > 240);   // angle ~0
    CHECK(k.pixel(10, 50).g > 240);   // angle ~pi
    Image tile(2, 2);
    tile(0, 0) = tile(1, 1) = Color{255, 255, 255};
    tile(1, 0) = tile(0, 1) = Color{0, 0, 0};
    Canvas pt = blank(8, 8);
    pt.set_fill(pattern(tile, Transform::scale(2, 2), true, true, SampleFilter::nearest));
    pt.fill_rect(0, 0, 8, 8);
    CHECK_EQ(pt.pixel(0, 0).r, 255);
    CHECK_EQ(pt.pixel(2, 0).r, 0);
    CHECK_EQ(pt.pixel(4, 0).r, 255);
    CHECK_THROWS_AS(Gradient{}.add_stop(1.5f, Color::black), GraphicsException);
}

FLYG_TEST(graphics_canvas_blending) {
    auto blend = [](Color dst, Color src, BlendMode m) { return composite(dst, src.to_float(), m); };
    const Color d{200, 100, 50, 255}, s{100, 200, 150, 255};
    CHECK(blend(d, s, BlendMode::normal) == s);
    const Color mul = blend(d, s, BlendMode::multiply);
    CHECK_EQ(mul.r, 78);   // 200*100/255
    CHECK_EQ(mul.g, 78);
    const Color scr = blend(d, s, BlendMode::screen);
    CHECK_EQ(scr.r, 222);  // 255 - 55*155/255 ~ 221.6
    CHECK(blend(d, s, BlendMode::difference) == (Color{100, 100, 100, 255}));
    CHECK(blend(d, s, BlendMode::darken) == (Color{100, 100, 50, 255}));
    CHECK(blend(d, s, BlendMode::lighten) == (Color{200, 200, 150, 255}));
    // half transparent source over opaque white
    const Color half = composite(Color::white, ColorF{0, 0, 0, 0.5f}, BlendMode::normal);
    CHECK(std::abs(int(half.r) - 128) <= 1);
    // Porter-Duff
    CHECK_EQ(blend(d, s, BlendMode::clear).a, 0);
    CHECK(blend(d, s, BlendMode::source) == s);
    CHECK(blend(d, s, BlendMode::dest_over) == d);
    CHECK_EQ(blend(Color{0, 0, 0, 0}, s, BlendMode::source_in).a, 0);
    CHECK_EQ(blend(d, s, BlendMode::xor_).a, 0);
    CHECK(blend(d, s, BlendMode::source_atop) == s);
    CHECK_EQ(blend(Color{250, 250, 250, 255}, Color{10, 10, 10, 255}, BlendMode::plus).r, 255);
    // transparent backdrop: every separable mode degrades to plain source
    for (auto m : {BlendMode::multiply, BlendMode::overlay, BlendMode::hue, BlendMode::soft_light})
        CHECK(blend(Color{0, 0, 0, 0}, s, m) == s);
    // canvas level: clear_rect punches a hole, global alpha scales
    Canvas c(20, 20, Color::white);
    c.clear_rect(5, 5, 10, 10);
    CHECK_EQ(c.pixel(10, 10).a, 0);
    CHECK_EQ(c.pixel(2, 2).a, 255);
    c.set_global_alpha(0.5f);
    c.set_fill(Color::black);
    c.fill_rect(0, 0, 3, 3);
    CHECK(std::abs(int(c.pixel(1, 1).r) - 128) <= 1);
}

FLYG_TEST(graphics_canvas_images) {
    Image src(4, 4, Color{255, 0, 0, 255});
    Canvas c = blank(20, 20);
    c.draw_image(src, 5, 5);
    CHECK_EQ(c.pixel(6, 6).r, 255);
    CHECK_EQ(c.pixel(4, 6).a, 0);
    CHECK_EQ(c.pixel(9, 6).a, 0);
    c.draw_image(src, Rect{12, 12, 8, 8}, SampleFilter::nearest);
    CHECK_EQ(c.pixel(19, 19).r, 255);
    Image big(200, 200, Color{0, 0, 255, 255});
    Canvas d = blank(20, 20);
    d.draw_image(big, Rect{0, 0, 20, 20});  // heavy minification path
    CHECK_EQ(d.pixel(10, 10).b, 255);
    CHECK_EQ(d.pixel(0, 0).a, 255);
    CHECK_THROWS_AS(c.draw_image(src, Rect{0, 0, 10, 10}, Rect{0, 0, 5, 5}), GraphicsException);
}

FLYG_TEST(graphics_path_svg_and_geometry) {
    const Path p = Path::parse_svg("M10 10 h 20 v 20 h -20 z m 50 0 l 10 0 10 10 z");
    const auto lines = p.flatten();
    CHECK_EQ(lines.size(), 2u);
    CHECK(lines[0].closed);
    CHECK_EQ(lines[0].pts[2].x, 30.0f);
    CHECK_EQ(lines[1].pts[0].x, 60.0f);  // relative moveto after close starts from the subpath start
    // arc: a half circle of radius 10 from (0,0) to (20,0)
    const Path arc = Path::parse_svg("M0 0 A10 10 0 0 1 20 0");
    const Rect b = arc.bounds();
    CHECK(b.w >= 19.9f);
    float min_y = 0;
    for (const auto& pl : arc.flatten(0.01f)) for (const Point& q : pl.pts) min_y = std::min(min_y, q.y);
    CHECK(std::fabs(min_y + 10.0f) < 0.05f);
    CHECK_THROWS_AS(Path::parse_svg("L 1 2"), GraphicsException);
    CHECK_THROWS_AS(Path::parse_svg("M 1"), GraphicsException);
    CHECK_THROWS_AS(Path::parse_svg("M 0 0 X 1"), GraphicsException);
    Path circle;
    circle.circle(0, 0, 50);
    CHECK(std::fabs(std::fabs(signed_area(circle)) - 3.14159265 * 2500) < 0.01 * 3.14159265 * 2500);
    Path rr;
    rr.rounded_rect(0, 0, 100, 50, 10);
    CHECK(std::fabs(std::fabs(signed_area(rr)) - (5000.0 - (4 - 3.14159265) * 100)) < 30.0);
}

FLYG_TEST(graphics_font_truetype) {
    const auto data = make_font(true);
    const Font f(data);
    CHECK_EQ(f.units_per_em(), 1000u);
    CHECK_EQ(f.glyph_count(), 3u);
    CHECK_EQ(f.glyph_index(U'A'), 1u);
    CHECK_EQ(f.glyph_index(U'B'), 2u);
    CHECK_EQ(f.glyph_index(U'Z'), 0u);
    CHECK_EQ(f.advance_units(1), 700u);
    CHECK_EQ(f.advance(2, 100.0f), 80.0f);
    CHECK_EQ(f.kerning_units(1, 2), -80);
    CHECK_EQ(f.ascent(1000.0f), 800.0f);
    CHECK(std::fabs(f.measure("AB", 10.0f) - (7.0f + 8.0f - 0.8f)) < 1e-4f);
    CHECK(std::fabs(f.measure("AB", 10.0f, false) - 15.0f) < 1e-4f);
    CHECK(std::fabs(std::fabs(signed_area(f.glyph_path(0))) - 1e6) < 1.0);
    CHECK(std::fabs(std::fabs(signed_area(f.glyph_path(1))) - 333333.0) < 3000.0);       // quadratic arch
    const Path comp = f.glyph_path(2);                                                   // scaled + translated composite
    CHECK(std::fabs(std::fabs(signed_area(comp)) - 83333.0) < 1000.0);
    const Rect b = comp.bounds();
    CHECK(std::fabs(b.x - 100) < 1 && std::fabs(b.y - 200) < 1 && std::fabs(b.right() - 600) < 1);
    // rendering: 'A' at 100px, baseline y=80 -> arch occupies x 0..70, y up to 80-100
    Canvas c = blank(120, 120);
    c.set_fill(Color::black);
    c.fill_text(f, "A", 10, 110, 100);
    CHECK_EQ(c.pixel(45, 105).a, 255);  // near the baseline inside the arch
    CHECK_EQ(c.pixel(45, 20).a, 0);     // above the apex
    CHECK(alpha_sum(c.image()) > 100 * 100 * 0.25);
    // corrupt / unsupported data
    auto bad = data;
    bad.resize(40);
    CHECK_THROWS_AS(Font{bad}, GraphicsException);
    auto otto = data;
    otto[0] = std::byte{'O'}, otto[1] = std::byte{'T'}, otto[2] = std::byte{'T'}, otto[3] = std::byte{'O'};
    CHECK_THROWS_AS(Font{otto}, GraphicsException);
    CHECK_EQ(utf8_to_codepoints("A\xC3\xA9\xE2\x82\xAC").size(), 3u);
    CHECK_EQ(utf8_to_codepoints("\xFF").front(), U'�');
}

FLYG_TEST(graphics_font_bitmap) {
    CHECK_EQ(BitmapFont::glyph(U'A')[0], 0x7C);
    CHECK_EQ(BitmapFont::glyph(U'€')[0], BitmapFont::glyph(U'?')[0]);
    CHECK_EQ(BitmapFont::measure("abc", 2.0f), 36.0f);
    Canvas c = blank(30, 12);
    c.set_fill(Color::black);
    c.fill_text("I", 0, 0, 1.0f);
    // 'I' columns: 00 41 7F 41 00 -> centre column fully lit (7 rows), serifs at rows 0 and 6
    CHECK_EQ(c.pixel(2, 3).a, 255);
    CHECK_EQ(c.pixel(1, 0).a, 255);
    CHECK_EQ(c.pixel(1, 3).a, 0);
    CHECK_EQ(c.pixel(0, 0).a, 0);
}

FLYG_TEST(graphics_canvas_robustness) {
    Canvas c = blank(50, 50);
    c.set_fill(Color::black);
    Path wild;
    wild.move_to(-1e9f, -1e9f).line_to(1e9f, 5).line_to(25, 1e9f).close();
    c.fill(wild);
    Path nan;
    nan.move_to(std::nanf(""), 1).line_to(5, 5).line_to(1, 9).close();
    c.fill(nan);
    c.scale(0, 0);
    c.fill_rect(0, 0, 10, 10);
    c.set_stroke(linear_gradient({0, 0}, {0, 0}, Gradient{{0, Color::black}}));
    c.draw_line(0, 0, 10, 10);
    CHECK_THROWS_AS(Canvas(0, 5), GraphicsException);
    CHECK_NOTHROW(blank().stroke(Path().move_to(0, 0).line_to(5, 5).line_to(9, 0)));
}
