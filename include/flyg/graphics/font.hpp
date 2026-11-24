// flyG - graphics module: fonts. TrueType (glyf) parser producing vector outlines, plus a built-in 5x7 bitmap font.
// Supported: .ttf / first font of .ttc, cmap formats 0/4/6/12, composite glyphs, hmtx, 'kern' format 0.
// Not supported: OpenType/CFF outlines (.otf with 'CFF '), GPOS kerning, shaping/bidi (text is laid out left to right).
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <flyg/graphics/path.hpp>

namespace flyg::graphics {

namespace font_detail {
inline char32_t next_codepoint(std::string_view s, usize& i) {
    const u8 c = static_cast<u8>(s[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
    if (extra < 0 || c >= 0xf8) return 0xfffd;
    char32_t cp = c & (0x3f >> extra);
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (static_cast<u8>(s[i]) & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | (static_cast<u8>(s[i++]) & 0x3f);
    }
    return cp;
}
}  // namespace font_detail

/// Decode a UTF-8 string into code points (invalid sequences become U+FFFD).
[[nodiscard]] inline std::vector<char32_t> utf8_to_codepoints(std::string_view s) {
    std::vector<char32_t> out;
    for (usize i = 0; i < s.size();) out.push_back(font_detail::next_codepoint(s, i));
    return out;
}

class Font {
public:
    /// Parse a font file image (copied).
    explicit Font(std::span<const std::byte> data) : data_(reinterpret_cast<const u8*>(data.data()), reinterpret_cast<const u8*>(data.data()) + data.size()) { parse(); }

    [[nodiscard]] u16 units_per_em() const noexcept { return upem_; }
    [[nodiscard]] u16 glyph_count() const noexcept { return num_glyphs_; }
    /// Ascent/descent/line gap in em units (descent is negative).
    [[nodiscard]] float ascent(float size) const noexcept { return static_cast<float>(ascent_) * size / upem_; }
    [[nodiscard]] float descent(float size) const noexcept { return static_cast<float>(descent_) * size / upem_; }
    [[nodiscard]] float line_gap(float size) const noexcept { return static_cast<float>(line_gap_) * size / upem_; }
    [[nodiscard]] float line_height(float size) const noexcept { return ascent(size) - descent(size) + line_gap(size); }

    /// Glyph index for a code point (0 = .notdef when unmapped).
    [[nodiscard]] u16 glyph_index(char32_t cp) const {
        for (const auto& st : cmaps_) {
            const u16 g = lookup(st, cp);
            if (g) return g;
        }
        return 0;
    }
    [[nodiscard]] bool has_glyph(char32_t cp) const { return glyph_index(cp) != 0; }
    /// Horizontal advance in font units.
    [[nodiscard]] u16 advance_units(u16 gid) const {
        if (num_hmetrics_ == 0) return 0;
        const u16 i = std::min<u16>(gid, static_cast<u16>(num_hmetrics_ - 1));
        return rd16(hmtx_ + usize(i) * 4);
    }
    [[nodiscard]] float advance(u16 gid, float size) const { return static_cast<float>(advance_units(gid)) * size / upem_; }
    /// Kerning adjustment in font units between two glyphs (0 when the font has no 'kern' pair).
    [[nodiscard]] i16 kerning_units(u16 left, u16 right) const {
        const auto it = kern_.find((u32(left) << 16) | right);
        return it == kern_.end() ? i16{0} : it->second;
    }

    /// Outline in font units, y axis up, quadratic curves. Empty path for blank glyphs.
    [[nodiscard]] const Path& glyph_path(u16 gid) const {
        auto it = cache_.find(gid);
        if (it != cache_.end()) return it->second;
        Path p;
        build_glyph(gid, p, Transform{}, 0);
        return cache_.emplace(gid, std::move(p)).first->second;
    }

    /// Width of a single line of text.
    [[nodiscard]] float measure(std::string_view text, float size, bool kerning = true) const {
        float x = 0;
        u16 prev = 0;
        for (usize i = 0; i < text.size();) {
            const u16 g = glyph_index(font_detail::next_codepoint(text, i));
            if (kerning && prev) x += static_cast<float>(kerning_units(prev, g)) * size / upem_;
            x += advance(g, size);
            prev = g;
        }
        return x;
    }

    /// Outline of a text run with its baseline origin at (x,y), y axis down (screen space). '\n' starts a new line.
    [[nodiscard]] Path text_path(std::string_view text, float x, float y, float size, bool kerning = true) const {
        Path out;
        const float s = size / upem_;
        float pen = x, base = y;
        u16 prev = 0;
        for (usize i = 0; i < text.size();) {
            const char32_t cp = font_detail::next_codepoint(text, i);
            if (cp == U'\n') { pen = x; base += line_height(size); prev = 0; continue; }
            const u16 g = glyph_index(cp);
            if (kerning && prev) pen += static_cast<float>(kerning_units(prev, g)) * s;
            const Path& gp = glyph_path(g);
            if (!gp.empty()) out.append(gp.transformed(Transform{s, 0, 0, -s, pen, base}));
            pen += advance(g, size);
            prev = g;
        }
        return out;
    }

private:
    struct CmapTable { u32 offset; u16 format; };

    [[nodiscard]] u16 rd16(usize o) const { if (o + 2 > data_.size()) bad("read past end of font"); return static_cast<u16>((data_[o] << 8) | data_[o + 1]); }
    [[nodiscard]] i16 rd16s(usize o) const { return static_cast<i16>(rd16(o)); }
    [[nodiscard]] u32 rd32(usize o) const { if (o + 4 > data_.size()) bad("read past end of font"); return (u32(data_[o]) << 24) | (u32(data_[o + 1]) << 16) | (u32(data_[o + 2]) << 8) | data_[o + 3]; }
    [[noreturn]] static void bad(const char* m) { throw GraphicsException(std::string("font: ") + m); }

    [[nodiscard]] u32 table(const char* tag, u32* len = nullptr) const {
        for (u16 i = 0; i < num_tables_; ++i) {
            const usize e = dir_ + 12 + usize(i) * 16;
            if (e + 16 > data_.size()) break;
            if (std::memcmp(&data_[e], tag, 4) == 0) {
                const u32 off = rd32(e + 8), l = rd32(e + 12);
                if (off > data_.size() || l > data_.size() - off) bad("table out of range");
                if (len) *len = l;
                return off;
            }
        }
        return 0;
    }

    void parse() {
        if (data_.size() < 12) bad("file too short");
        usize base = 0;
        if (std::memcmp(data_.data(), "ttcf", 4) == 0) base = rd32(12);
        dir_ = base;
        const u32 tag = rd32(dir_);
        if (tag == 0x4f54544f) bad("OpenType/CFF fonts are not supported (TrueType 'glyf' outlines only)");
        if (tag != 0x00010000 && tag != 0x74727565) bad("not a TrueType font");
        num_tables_ = rd16(dir_ + 4);
        const u32 head = table("head");
        if (!head) bad("missing 'head' table");
        upem_ = rd16(head + 18);
        if (upem_ == 0) bad("invalid unitsPerEm");
        loca_long_ = rd16(head + 50) != 0;
        const u32 maxp = table("maxp");
        if (!maxp) bad("missing 'maxp' table");
        num_glyphs_ = rd16(maxp + 4);
        loca_ = table("loca", &loca_len_);
        glyf_ = table("glyf", &glyf_len_);
        if (!loca_ || !glyf_) bad("missing 'glyf'/'loca' (not a TrueType-outline font)");
        const usize need = (usize(num_glyphs_) + 1) * (loca_long_ ? 4 : 2);
        if (loca_len_ < need) bad("'loca' table too short");
        const u32 hhea = table("hhea");
        if (!hhea) bad("missing 'hhea' table");
        ascent_ = rd16s(hhea + 4), descent_ = rd16s(hhea + 6), line_gap_ = rd16s(hhea + 8);
        num_hmetrics_ = rd16(hhea + 34);
        u32 hl = 0;
        const u32 hm = table("hmtx", &hl);
        if (!hm || hl < usize(num_hmetrics_) * 4) bad("missing or short 'hmtx' table");
        hmtx_ = hm;
        // cmap
        const u32 cmap = table("cmap");
        if (cmap) {
            const u16 n = rd16(cmap + 2);
            std::vector<std::pair<int, CmapTable>> found;
            for (u16 i = 0; i < n; ++i) {
                const u16 pid = rd16(cmap + 4 + usize(i) * 8), eid = rd16(cmap + 6 + usize(i) * 8);
                const u32 off = cmap + rd32(cmap + 8 + usize(i) * 8);
                const u16 fmt = rd16(off);
                if (fmt != 0 && fmt != 4 && fmt != 6 && fmt != 12) continue;
                int pri = 9;
                if (pid == 3 && eid == 10) pri = 0;
                else if (pid == 0 && (eid == 4 || eid == 6)) pri = 1;
                else if (pid == 3 && eid == 1) pri = 2;
                else if (pid == 0) pri = 3;
                else if (pid == 1 && eid == 0) pri = 5;
                found.push_back({pri, {off, fmt}});
            }
            std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& f : found) cmaps_.push_back(f.second);
        }
        // kern (format 0 subtables only)
        const u32 kern = table("kern");
        if (kern) {
            const u16 nt = rd16(kern + 2);
            usize p = kern + 4;
            for (u16 t = 0; t < nt; ++t) {
                const u16 len = rd16(p + 2), cov = rd16(p + 4);
                if ((cov >> 8) == 0 && (cov & 1)) {
                    const u16 np = rd16(p + 6);
                    for (u16 k = 0; k < np && p + 14 + usize(k) * 6 + 6 <= data_.size(); ++k) {
                        const usize q = p + 14 + usize(k) * 6;
                        kern_[(u32(rd16(q)) << 16) | rd16(q + 2)] = rd16s(q + 4);
                    }
                }
                if (len == 0) break;
                p += len;
            }
        }
        if (glyph_count() == 0) bad("font has no glyphs");
    }

    [[nodiscard]] u16 lookup(const CmapTable& t, char32_t cp) const {
        const usize o = t.offset;
        switch (t.format) {
            case 0: return cp < 256 ? (o + 6 + cp < data_.size() ? data_[o + 6 + cp] : u16{0}) : u16{0};
            case 6: {
                const u16 first = rd16(o + 6), cnt = rd16(o + 8);
                return cp >= first && cp < u32(first) + cnt ? rd16(o + 10 + usize(cp - first) * 2) : u16{0};
            }
            case 4: {
                if (cp > 0xffff) return 0;
                const u16 segx2 = rd16(o + 6);
                const usize ends = o + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;
                for (usize s = 0; s < segx2; s += 2) {
                    if (rd16(ends + s) < cp) continue;
                    const u16 st = rd16(starts + s);
                    if (st > cp) return 0;
                    const u16 ro = rd16(ranges + s), delta = rd16(deltas + s);
                    if (ro == 0) return static_cast<u16>(cp + delta);
                    const u16 g = rd16(ranges + s + ro + usize(cp - st) * 2);
                    return g ? static_cast<u16>(g + delta) : u16{0};
                }
                return 0;
            }
            default: {  // 12
                const u32 ng = rd32(o + 12);
                usize lo = 0, hi = ng;
                while (lo < hi) {
                    const usize mid = (lo + hi) / 2, g = o + 16 + mid * 12;
                    if (cp < rd32(g)) hi = mid;
                    else if (cp > rd32(g + 4)) lo = mid + 1;
                    else return static_cast<u16>(rd32(g + 8) + (cp - rd32(g)));
                }
                return 0;
            }
        }
    }

    void build_glyph(u16 gid, Path& out, const Transform& t, int depth) const {
        if (gid >= num_glyphs_ || depth > 8) return;
        const usize li = usize(gid);
        const u32 a = loca_long_ ? rd32(loca_ + li * 4) : u32(rd16(loca_ + li * 2)) * 2;
        const u32 b = loca_long_ ? rd32(loca_ + li * 4 + 4) : u32(rd16(loca_ + li * 2 + 2)) * 2;
        if (b <= a) return;  // blank glyph
        if (b > glyf_len_ || a + 10 > glyf_len_) bad("glyph out of range");
        const usize g = glyf_ + a, gend = glyf_ + b;
        const i16 ncont = rd16s(g);
        if (ncont >= 0) {
            usize p = g + 10;
            std::vector<u16> ends(static_cast<usize>(ncont));
            for (auto& e : ends) { e = rd16(p); p += 2; }
            const usize npts = ncont ? usize(ends.back()) + 1 : 0;
            for (usize i = 1; i < ends.size(); ++i) if (ends[i] < ends[i - 1]) bad("corrupt glyph contours");
            const u16 ilen = rd16(p);
            p += 2 + ilen;
            std::vector<u8> flags;
            flags.reserve(npts);
            while (flags.size() < npts) {
                if (p >= gend) bad("glyph flags truncated");
                const u8 f = data_[p++];
                flags.push_back(f);
                if (f & 8) {
                    if (p >= gend) bad("glyph flags truncated");
                    for (u8 r = data_[p++]; r && flags.size() < npts; --r) flags.push_back(f);
                }
            }
            std::vector<int> xs(npts), ys(npts);
            int v = 0;
            for (usize i = 0; i < npts; ++i) {
                const u8 f = flags[i];
                if (f & 2) { if (p >= gend) bad("glyph coordinates truncated"); const int d = data_[p++]; v += (f & 16) ? d : -d; }
                else if (!(f & 16)) { if (p + 2 > gend) bad("glyph coordinates truncated"); v += rd16s(p); p += 2; }
                xs[i] = v;
            }
            v = 0;
            for (usize i = 0; i < npts; ++i) {
                const u8 f = flags[i];
                if (f & 4) { if (p >= gend) bad("glyph coordinates truncated"); const int d = data_[p++]; v += (f & 32) ? d : -d; }
                else if (!(f & 32)) { if (p + 2 > gend) bad("glyph coordinates truncated"); v += rd16s(p); p += 2; }
                ys[i] = v;
            }
            usize start = 0;
            for (u16 e : ends) {
                const usize n = usize(e) - start + 1;
                emit_contour(out, t, &xs[start], &ys[start], &flags[start], n);
                start = usize(e) + 1;
            }
            return;
        }
        // composite
        usize p = g + 10;
        for (;;) {
            const u16 fl = rd16(p), sub = rd16(p + 2);
            p += 4;
            float dx, dy;
            if (fl & 1) { dx = rd16s(p), dy = rd16s(p + 2); p += 4; }
            else { dx = static_cast<i8>(data_[p]), dy = static_cast<i8>(data_[p + 1]); p += 2; }
            if (!(fl & 2)) dx = dy = 0;  // point-matching composites are not supported; place at the origin
            float m00 = 1, m01 = 0, m10 = 0, m11 = 1;
            auto f2 = [&](usize o) { return static_cast<float>(rd16s(o)) / 16384.0f; };
            if (fl & 8) { m00 = m11 = f2(p); p += 2; }
            else if (fl & 0x40) { m00 = f2(p), m11 = f2(p + 2); p += 4; }
            else if (fl & 0x80) { m00 = f2(p), m01 = f2(p + 2), m10 = f2(p + 4), m11 = f2(p + 6); p += 8; }
            // TrueType: x' = m00*x + m10*y + dx ; y' = m01*x + m11*y + dy
            const Transform local{m00, m01, m10, m11, dx, dy};
            build_glyph(sub, out, t * local, depth + 1);
            if (!(fl & 0x20)) break;
        }
    }

    static void emit_contour(Path& out, const Transform& t, const int* xs, const int* ys, const u8* fl, usize n) {
        if (n < 2) return;
        auto pt = [&](usize i) { return t({static_cast<float>(xs[i % n]), static_cast<float>(ys[i % n])}); };
        auto on = [&](usize i) { return (fl[i % n] & 1) != 0; };
        // find a starting on-curve point (or synthesise one between two off-curve points)
        usize first = 0;
        Point start;
        bool found = false;
        for (usize i = 0; i < n; ++i) if (on(i)) { first = i; found = true; break; }
        usize idx;
        if (found) { start = pt(first); idx = first + 1; }
        else { const Point a = pt(0), b = pt(1); start = (a + b) * 0.5f; first = 0; idx = 1; }
        out.move_to(start.x, start.y);
        Point ctrl{};
        bool have_ctrl = false;
        const usize count = found ? n - 1 : n;  // points left to visit after the start
        for (usize k = 0; k < count; ++k, ++idx) {
            const Point p = pt(idx);
            if (on(idx)) {
                if (have_ctrl) { out.quad_to(ctrl.x, ctrl.y, p.x, p.y); have_ctrl = false; }
                else out.line_to(p.x, p.y);
            } else {
                if (have_ctrl) {
                    const Point mid = (ctrl + p) * 0.5f;
                    out.quad_to(ctrl.x, ctrl.y, mid.x, mid.y);
                }
                ctrl = p;
                have_ctrl = true;
            }
        }
        if (have_ctrl) out.quad_to(ctrl.x, ctrl.y, start.x, start.y);
        out.close();
    }

    std::vector<u8> data_;
    usize dir_ = 0;
    u16 num_tables_ = 0, upem_ = 0, num_glyphs_ = 0, num_hmetrics_ = 0;
    bool loca_long_ = false;
    u32 loca_ = 0, loca_len_ = 0, glyf_ = 0, glyf_len_ = 0, hmtx_ = 0;
    i16 ascent_ = 0, descent_ = 0, line_gap_ = 0;
    std::vector<CmapTable> cmaps_;
    std::unordered_map<u32, i16> kern_;
    mutable std::unordered_map<u16, Path> cache_;
};

/// Built-in 5x7 pixel font covering printable ASCII (32..126); '?' is drawn for other characters.
class BitmapFont {
public:
    static constexpr int kGlyphWidth = 5, kGlyphHeight = 8, kAdvance = 6, kLineHeight = 9;

    /// Column bytes of a glyph (bit 0 = top row). Five columns.
    [[nodiscard]] static const std::array<u8, 5>& glyph(char32_t cp) {
        static const std::array<std::array<u8, 5>, 95> table = {{
            {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
            {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00},
            {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
            {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
            {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33},
            {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
            {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00}, {0x00, 0x40, 0x34, 0x00, 0x00},
            {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06},
            {0x3E, 0x41, 0x5D, 0x59, 0x4E}, {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
            {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x41, 0x51, 0x73},
            {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
            {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
            {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x26, 0x49, 0x49, 0x49, 0x32},
            {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
            {0x63, 0x14, 0x08, 0x14, 0x63}, {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
            {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x41, 0x7F}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
            {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40}, {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28},
            {0x38, 0x44, 0x44, 0x28, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78},
            {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
            {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
            {0xFC, 0x18, 0x24, 0x24, 0x18}, {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
            {0x04, 0x04, 0x3F, 0x44, 0x24}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
            {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
            {0x00, 0x00, 0x77, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
        }};
        return table[(cp >= 32 && cp <= 126) ? cp - 32 : '?' - 32];
    }

    /// Outline of the text as pixel squares (scale = device pixels per font pixel); baseline-less: (x,y) is the top-left corner.
    [[nodiscard]] static Path text_path(std::string_view text, float x, float y, float scale = 1.0f) {
        Path p;
        float pen = x, top = y;
        for (usize i = 0; i < text.size();) {
            const char32_t cp = font_detail::next_codepoint(text, i);
            if (cp == U'\n') { pen = x; top += kLineHeight * scale; continue; }
            const auto& g = glyph(cp);
            for (int cx = 0; cx < 5; ++cx)
                for (int cy = 0; cy < 8; ++cy)
                    if (g[static_cast<usize>(cx)] & (1u << cy))
                        p.rect(pen + static_cast<float>(cx) * scale, top + static_cast<float>(cy) * scale, scale, scale);
            pen += kAdvance * scale;
        }
        return p;
    }
    [[nodiscard]] static float measure(std::string_view text, float scale = 1.0f) {
        float best = 0, cur = 0;
        for (usize i = 0; i < text.size();) {
            const char32_t cp = font_detail::next_codepoint(text, i);
            if (cp == U'\n') { cur = 0; continue; }
            cur += kAdvance * scale;
            best = std::max(best, cur);
        }
        return best;
    }
};

}  // namespace flyg::graphics
