// flyG - TOML 1.0: parser and writer
// SPDX-License-Identifier: GPL-2.0-only
//
// All TOML 1.0 syntax is accepted, with full duplicate-key / table-redefinition checking.
// The Value model has no date-time type: TOML date-times, dates and times are validated for shape
// and returned as strings (e.g. "1979-05-27T07:32:00Z"); strings are written back quoted.
// TOML has no null: writing a null throws unless WriteOptions::skip_null is set (tables only).
#pragma once

#include <list>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data::toml {

namespace detail {

struct TNode {
    enum class Kind { table, array_of_tables, value };
    Kind kind = Kind::table;
    bool defined = false;  // table introduced by an explicit [header]
    bool dotted = false;   // table created implicitly by a dotted key
    bool frozen = false;   // inline table: closed for further changes
    std::list<std::pair<std::string, TNode>> kids;
    std::vector<TNode> items;  // array_of_tables elements (each a table)
    Value val;                 // value kind

    TNode* find(const std::string& k) {
        for (auto& e : kids)
            if (e.first == k) return &e.second;
        return nullptr;
    }
};

[[nodiscard]] inline Value to_value(const TNode& n) {
    switch (n.kind) {
        case TNode::Kind::value: return n.val;
        case TNode::Kind::table: {
            Object o;
            o.reserve(n.kids.size());
            for (const auto& [k, c] : n.kids) o.append_unchecked(k, to_value(c));
            return Value(std::move(o));
        }
        case TNode::Kind::array_of_tables: {
            Array a;
            for (const auto& it : n.items) a.push_back(to_value(it));
            return Value(std::move(a));
        }
    }
    return Value();
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value parse() {
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
        TNode root;
        TNode* current = &root;
        for (;;) {
            skip_ws_comments_newlines();
            if (pos_ >= s_.size()) break;
            if (cur() == '[') {
                current = header(root);
            } else {
                key_value(*current);
            }
            end_of_statement();
        }
        return to_value(root);
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        usize line = 1, col = 1;
        for (usize i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw DataException("TOML: " + what + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")", pos_);
    }
    [[nodiscard]] char cur() const noexcept { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    [[nodiscard]] char at(usize i) const noexcept { return i < s_.size() ? s_[i] : '\0'; }
    void skip_ws() {
        while (cur() == ' ' || cur() == '\t') ++pos_;
    }
    void skip_comment() {
        if (cur() == '#') {
            while (pos_ < s_.size() && s_[pos_] != '\n') {
                const auto c = static_cast<unsigned char>(s_[pos_]);
                if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7f) fail("control character in comment");
                ++pos_;
            }
        }
    }
    void skip_ws_comments_newlines() {
        for (;;) {
            skip_ws();
            skip_comment();
            if (cur() == '\n') { ++pos_; continue; }
            if (cur() == '\r' && at(pos_ + 1) == '\n') { pos_ += 2; continue; }
            return;
        }
    }
    void end_of_statement() {
        skip_ws();
        skip_comment();
        if (pos_ >= s_.size()) return;
        if (cur() == '\n') { ++pos_; return; }
        if (cur() == '\r' && at(pos_ + 1) == '\n') { pos_ += 2; return; }
        fail("expected a newline after the statement");
    }

    // ---- keys ----------------------------------------------------------------------------------
    [[nodiscard]] static bool bare_char(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    std::string simple_key() {
        const char c = cur();
        if (c == '"') return basic_string(false);
        if (c == '\'') return literal_string(false);
        const usize b = pos_;
        while (bare_char(cur())) ++pos_;
        if (pos_ == b) fail("key expected");
        return std::string(s_.substr(b, pos_ - b));
    }
    std::vector<std::string> dotted_key() {
        std::vector<std::string> parts;
        for (;;) {
            skip_ws();
            parts.push_back(simple_key());
            skip_ws();
            if (cur() == '.') { ++pos_; continue; }
            return parts;
        }
    }

    // ---- headers -------------------------------------------------------------------------------
    TNode* header(TNode& root) {
        const bool aot = at(pos_ + 1) == '[';
        pos_ += aot ? 2 : 1;
        const auto path = dotted_key();
        skip_ws();
        if (cur() != ']') fail("expected ']'");
        ++pos_;
        if (aot) {
            if (cur() != ']') fail("expected ']]'");
            ++pos_;
        }
        TNode* t = &root;
        for (usize i = 0; i + 1 < path.size(); ++i) {
            if (t->frozen) fail("cannot extend an inline table");
            TNode* c = t->find(path[i]);
            if (!c) {
                t->kids.emplace_back(path[i], TNode{});
                c = &t->kids.back().second;
            }
            t = descend(c, path[i]);
        }
        if (t->frozen) fail("cannot extend an inline table");
        const std::string& last = path.back();
        TNode* c = t->find(last);
        if (aot) {
            if (!c) {
                t->kids.emplace_back(last, TNode{});
                c = &t->kids.back().second;
                c->kind = TNode::Kind::array_of_tables;
            } else if (c->kind != TNode::Kind::array_of_tables) {
                fail("'" + last + "' is already defined and is not an array of tables");
            }
            c->items.emplace_back();
            c->items.back().defined = true;
            return &c->items.back();
        }
        if (!c) {
            t->kids.emplace_back(last, TNode{});
            c = &t->kids.back().second;
            c->defined = true;
            return c;
        }
        if (c->kind != TNode::Kind::table || c->defined || c->dotted || c->frozen) fail("table '" + last + "' is already defined");
        c->defined = true;
        return c;
    }

    TNode* descend(TNode* c, const std::string& name) {
        switch (c->kind) {
            case TNode::Kind::table:
                if (c->frozen) fail("cannot extend an inline table");
                return c;
            case TNode::Kind::array_of_tables: return &c->items.back();
            case TNode::Kind::value: fail("'" + name + "' is already defined as a value");
        }
        return c;
    }

    // ---- key = value ---------------------------------------------------------------------------
    void key_value(TNode& table) {
        const auto path = dotted_key();
        skip_ws();
        if (cur() != '=') fail("expected '=' after the key");
        ++pos_;
        skip_ws();
        TNode* t = &table;
        for (usize i = 0; i + 1 < path.size(); ++i) {
            if (t->frozen) fail("cannot extend an inline table");
            TNode* c = t->find(path[i]);
            if (!c) {
                t->kids.emplace_back(path[i], TNode{});
                c = &t->kids.back().second;
                c->dotted = true;
            } else if (c->kind != TNode::Kind::table || c->frozen || !c->dotted) {
                // only tables created by dotted keys of this same table may be extended this way
                fail("key '" + path[i] + "' is already defined");
            }
            t = c;
        }
        if (t->frozen) fail("cannot extend an inline table");
        const std::string& last = path.back();
        if (t->find(last)) fail("duplicate key '" + last + "'");
        TNode v;
        if (cur() == '{') {
            v = inline_table();
        } else {
            v.kind = TNode::Kind::value;
            v.val = value(0);
        }
        t->kids.emplace_back(last, std::move(v));
    }

    TNode inline_table() {
        ++pos_;  // {
        TNode t;
        t.frozen = true;
        skip_ws();
        if (cur() == '}') { ++pos_; return t; }
        for (;;) {
            skip_ws();
            // inline tables are frozen only once closed, so temporarily open the one being filled
            t.frozen = false;
            key_value(t);
            t.frozen = true;
            skip_ws();
            if (cur() == ',') { ++pos_; skip_ws(); if (cur() == '}') fail("trailing comma in an inline table"); continue; }
            if (cur() == '}') { ++pos_; break; }
            fail("expected ',' or '}' in an inline table");
        }
        return t;
    }

    // ---- values --------------------------------------------------------------------------------
    Value value(usize depth) {
        if (depth > 256) fail("maximum nesting depth exceeded");
        const char c = cur();
        if (c == '"') {
            if (at(pos_ + 1) == '"' && at(pos_ + 2) == '"') return Value(multiline_basic());
            return Value(basic_string(false));
        }
        if (c == '\'') {
            if (at(pos_ + 1) == '\'' && at(pos_ + 2) == '\'') return Value(multiline_literal());
            return Value(literal_string(false));
        }
        if (c == '[') return array(depth);
        if (c == '{') return to_value(inline_table());
        if (c == 't' && s_.compare(pos_, 4, "true") == 0 && !bare_char(at(pos_ + 4))) { pos_ += 4; return Value(true); }
        if (c == 'f' && s_.compare(pos_, 5, "false") == 0 && !bare_char(at(pos_ + 5))) { pos_ += 5; return Value(false); }
        return number_or_datetime();
    }

    Value array(usize depth) {
        ++pos_;  // [
        Array a;
        for (;;) {
            skip_ws_comments_newlines();
            if (cur() == ']') { ++pos_; break; }
            if (pos_ >= s_.size()) fail("unterminated array");
            a.push_back(value(depth + 1));
            skip_ws_comments_newlines();
            if (cur() == ',') { ++pos_; continue; }
            if (cur() == ']') { ++pos_; break; }
            fail("expected ',' or ']' in an array");
        }
        return Value(std::move(a));
    }

    char32_t hex_escape(int n) {
        char32_t v = 0;
        for (int i = 0; i < n; ++i) {
            const char h = cur();
            int d;
            if (h >= '0' && h <= '9') d = h - '0';
            else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
            else fail("invalid unicode escape");
            v = (v << 4) | static_cast<char32_t>(d);
            ++pos_;
        }
        if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) fail("unicode escape is not a scalar value");
        return v;
    }

    void escape(std::string& out, bool multiline) {
        // after the backslash
        const char e = cur();
        ++pos_;
        switch (e) {
            case 'b': out += '\b'; return;
            case 't': out += '\t'; return;
            case 'n': out += '\n'; return;
            case 'f': out += '\f'; return;
            case 'r': out += '\r'; return;
            case '"': out += '"'; return;
            case '\\': out += '\\'; return;
            case 'u': utf8::append(out, hex_escape(4)); return;
            case 'U': utf8::append(out, hex_escape(8)); return;
            default: break;
        }
        if (multiline && (e == ' ' || e == '\t' || e == '\n' || e == '\r')) {
            // line-ending backslash: skip the whitespace and newlines that follow
            --pos_;
            usize p = pos_;
            while (at(p) == ' ' || at(p) == '\t') ++p;
            if (at(p) == '\r' && at(p + 1) == '\n') ++p;
            if (at(p) != '\n') { pos_ = p; fail("invalid escape sequence"); }
            while (at(p) == ' ' || at(p) == '\t' || at(p) == '\n' || at(p) == '\r') ++p;
            pos_ = p;
            return;
        }
        --pos_;
        fail(std::string("invalid escape sequence '\\") + e + "'");
    }

    void check_char(unsigned char c, bool allow_newline) {
        if ((c < 0x20 && c != '\t' && !(allow_newline && (c == '\n' || c == '\r'))) || c == 0x7f)
            fail("control character in string");
    }

    std::string basic_string(bool) {
        ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= s_.size() || cur() == '\n') fail("unterminated string");
            const char c = cur();
            if (c == '"') { ++pos_; break; }
            if (c == '\\') { ++pos_; escape(out, false); continue; }
            check_char(static_cast<unsigned char>(c), false);
            out += c;
            ++pos_;
        }
        if (!utf8::is_valid(out)) fail("invalid UTF-8 in string");
        return out;
    }

    std::string literal_string(bool) {
        ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= s_.size() || cur() == '\n') fail("unterminated literal string");
            const char c = cur();
            if (c == '\'') { ++pos_; break; }
            check_char(static_cast<unsigned char>(c), false);
            out += c;
            ++pos_;
        }
        if (!utf8::is_valid(out)) fail("invalid UTF-8 in string");
        return out;
    }

    std::string multiline_basic() {
        pos_ += 3;
        if (cur() == '\r' && at(pos_ + 1) == '\n') pos_ += 2;
        else if (cur() == '\n') ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated multi-line string");
            if (cur() == '"' && at(pos_ + 1) == '"' && at(pos_ + 2) == '"') {
                // up to two further quotes belong to the content
                usize extra = 0;
                while (at(pos_ + 3 + extra) == '"' && extra < 2) ++extra;
                out.append(extra, '"');
                pos_ += 3 + extra;
                break;
            }
            const char c = cur();
            if (c == '\\') { ++pos_; escape(out, true); continue; }
            if (c == '\r' && at(pos_ + 1) == '\n') { out += '\n'; pos_ += 2; continue; }
            check_char(static_cast<unsigned char>(c), true);
            out += c;
            ++pos_;
        }
        if (!utf8::is_valid(out)) fail("invalid UTF-8 in string");
        return out;
    }

    std::string multiline_literal() {
        pos_ += 3;
        if (cur() == '\r' && at(pos_ + 1) == '\n') pos_ += 2;
        else if (cur() == '\n') ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated multi-line literal string");
            if (cur() == '\'' && at(pos_ + 1) == '\'' && at(pos_ + 2) == '\'') {
                usize extra = 0;
                while (at(pos_ + 3 + extra) == '\'' && extra < 2) ++extra;
                out.append(extra, '\'');
                pos_ += 3 + extra;
                break;
            }
            const char c = cur();
            if (c == '\r' && at(pos_ + 1) == '\n') { out += '\n'; pos_ += 2; continue; }
            check_char(static_cast<unsigned char>(c), true);
            out += c;
            ++pos_;
        }
        if (!utf8::is_valid(out)) fail("invalid UTF-8 in string");
        return out;
    }

    // ---- numbers and date-times ----------------------------------------------------------------
    [[nodiscard]] static bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

    bool matches_digits(usize p, int n) const {
        for (int i = 0; i < n; ++i)
            if (!digit(at(p + static_cast<usize>(i)))) return false;
        return true;
    }

    Value number_or_datetime() {
        const usize start = pos_;
        // date or date-time: 4 digits followed by '-' ; local time: 2 digits followed by ':'
        if (matches_digits(pos_, 4) && at(pos_ + 4) == '-') return datetime();
        if (matches_digits(pos_, 2) && at(pos_ + 2) == ':') return datetime();

        usize e = pos_;
        while (e < s_.size() && (bare_char(s_[e]) || s_[e] == '.' || s_[e] == '+')) ++e;
        std::string tok(s_.substr(pos_, e - pos_));
        if (tok.empty()) fail("value expected");
        pos_ = e;
        auto bad = [&]() -> Value {
            pos_ = start;
            fail("invalid value '" + tok + "'");
        };
        bool neg = false;
        std::string_view t = tok;
        if (t[0] == '+' || t[0] == '-') { neg = t[0] == '-'; t.remove_prefix(1); }
        if (t == "inf") return Value(neg ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
        if (t == "nan") return Value(std::numeric_limits<double>::quiet_NaN());
        if (t.empty()) return bad();

        // prefixed integers (no sign allowed)
        if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'o' || t[1] == 'b') && tok.size() == t.size()) {
            const int base = t[1] == 'x' ? 16 : t[1] == 'o' ? 8 : 2;
            std::string digits;
            bool last_us = true;
            for (usize i = 2; i < t.size(); ++i) {
                const char c = t[i];
                if (c == '_') { if (last_us) return bad(); last_us = true; continue; }
                last_us = false;
                digits += c;
            }
            if (last_us || digits.empty()) return bad();
            u64 v = 0;
            const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
            if (r.ec != std::errc() || r.ptr != digits.data() + digits.size() || v > static_cast<u64>(std::numeric_limits<i64>::max())) return bad();
            return Value(static_cast<i64>(v));
        }

        // decimal integer or float
        std::string clean;
        bool is_float = false;
        char prev = '\0';
        for (usize i = 0; i < t.size(); ++i) {
            const char c = t[i];
            if (c == '_') {
                if (!digit(prev) || i + 1 >= t.size() || !digit(t[i + 1])) return bad();
            } else {
                if (c == '.' || c == 'e' || c == 'E') is_float = true;
                clean += c;
            }
            prev = c;
        }
        // grammar check
        usize i = 0;
        auto digits_run = [&](bool allow_leading_zero_run) {
            const usize b = i;
            while (i < clean.size() && digit(clean[i])) ++i;
            if (!allow_leading_zero_run && i - b > 1 && clean[b] == '0') return static_cast<usize>(0);
            return i - b;
        };
        if (digits_run(false) == 0) return bad();
        if (i < clean.size() && clean[i] == '.') {
            ++i;
            if (digits_run(true) == 0) return bad();
        }
        if (i < clean.size() && (clean[i] == 'e' || clean[i] == 'E')) {
            ++i;
            if (i < clean.size() && (clean[i] == '+' || clean[i] == '-')) ++i;
            if (digits_run(true) == 0) return bad();
        }
        if (i != clean.size()) return bad();
        if (!is_float) {
            i64 v = 0;
            const auto r = std::from_chars(clean.data(), clean.data() + clean.size(), v);
            if (r.ec != std::errc() || r.ptr != clean.data() + clean.size()) {
                // allow exactly INT64_MIN
                if (neg && clean == "9223372036854775808") return Value(std::numeric_limits<i64>::min());
                pos_ = start;
                fail("integer out of range '" + tok + "'");
            }
            return Value(neg ? -v : v);
        }
        double d = 0;
        const auto r = std::from_chars(clean.data(), clean.data() + clean.size(), d);
        if (r.ptr != clean.data() + clean.size() || r.ec == std::errc::invalid_argument) return bad();
        if (r.ec == std::errc::result_out_of_range) {
            const auto ep = clean.find_first_of("eE");
            const bool under = ep != std::string::npos && ep + 1 < clean.size() && clean[ep + 1] == '-';
            d = under ? 0.0 : std::numeric_limits<double>::infinity();
        }
        return Value(neg ? -d : d);
    }

    Value datetime() {
        const usize start = pos_;
        auto num = [&](int n, int lo, int hi) {
            if (!matches_digits(pos_, n)) fail("malformed date or time");
            int v = 0;
            for (int i = 0; i < n; ++i) v = v * 10 + (s_[pos_ + static_cast<usize>(i)] - '0');
            pos_ += static_cast<usize>(n);
            if (v < lo || v > hi) fail("date or time field out of range");
            return v;
        };
        auto time_part = [&]() {
            num(2, 0, 23);
            if (cur() != ':') fail("malformed time");
            ++pos_;
            num(2, 0, 59);
            if (cur() != ':') fail("malformed time");
            ++pos_;
            num(2, 0, 60);
            if (cur() == '.') {
                ++pos_;
                if (!digit(cur())) fail("malformed fractional seconds");
                while (digit(cur())) ++pos_;
            }
        };
        bool has_date = false;
        if (matches_digits(pos_, 4) && at(pos_ + 4) == '-') {
            has_date = true;
            const int y = num(4, 0, 9999);
            ++pos_;  // '-'
            const int m = num(2, 1, 12);
            if (cur() != '-') fail("malformed date");
            ++pos_;
            const int d = num(2, 1, 31);
            static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
            if (d > days[m - 1] || (m == 2 && d == 29 && !leap)) fail("no such calendar day");
        }
        bool has_time = false;
        if (has_date) {
            if ((cur() == 'T' || cur() == 't') || (cur() == ' ' && matches_digits(pos_ + 1, 2) && at(pos_ + 3) == ':')) {
                ++pos_;
                time_part();
                has_time = true;
            }
        } else {
            time_part();
            has_time = true;
        }
        if (has_date && has_time) {
            if (cur() == 'Z' || cur() == 'z') {
                ++pos_;
            } else if (cur() == '+' || cur() == '-') {
                ++pos_;
                num(2, 0, 23);
                if (cur() != ':') fail("malformed UTC offset");
                ++pos_;
                num(2, 0, 59);
            }
        }
        std::string text(s_.substr(start, pos_ - start));
        for (char& c : text)
            if (c == 't') c = 'T';
            else if (c == 'z') c = 'Z';
        return Value(std::move(text));
    }

    std::string_view s_;
    usize pos_ = 0;
};

}  // namespace detail

[[nodiscard]] inline Value parse(std::string_view text) {
    if (!utf8::is_valid(text)) throw DataException("TOML: input is not valid UTF-8");
    return detail::Parser(text).parse();
}

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------
struct WriteOptions {
    bool skip_null = false;  ///< omit null members instead of throwing
};

namespace detail {

[[nodiscard]] inline std::string quote(std::string_view s) {
    if (!utf8::is_valid(s)) throw DataException("TOML: cannot write a string that is not valid UTF-8");
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\f': out += "\\f"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    static const char* hx = "0123456789abcdef";
                    out += "\\u00";
                    out += hx[c >> 4];
                    out += hx[c & 15];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] inline std::string key_text(std::string_view k) {
    bool bare = !k.empty();
    for (char c : k)
        if (!(((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))) bare = false;
    return bare ? std::string(k) : quote(k);
}

inline void write_inline(std::string& out, const Value& v, const WriteOptions& o) {
    switch (v.type()) {
        case Type::null: throw DataException("TOML: null cannot be represented");
        case Type::boolean: out += v.as_bool() ? "true" : "false"; break;
        case Type::integer: out += std::to_string(v.as_int()); break;
        case Type::real: {
            const double d = v.as_double();
            if (std::isnan(d)) { out += "nan"; break; }
            if (std::isinf(d)) { out += d < 0 ? "-inf" : "inf"; break; }
            char buf[40];
            const auto r = std::to_chars(buf, buf + sizeof buf, d);
            std::string_view sv(buf, static_cast<usize>(r.ptr - buf));
            out += sv;
            if (sv.find_first_of(".eE") == std::string_view::npos) out += ".0";
            break;
        }
        case Type::string: out += quote(v.as_string()); break;
        case Type::binary: throw DataException("TOML: binary values cannot be represented");
        case Type::array: {
            out += '[';
            bool first = true;
            for (const auto& x : v.as_array()) {
                if (!first) out += ", ";
                first = false;
                write_inline(out, x, o);
            }
            out += ']';
            break;
        }
        case Type::object: {
            std::string body;
            for (const auto& [k, x] : v.as_object()) {
                if (x.is_null() && o.skip_null) continue;
                if (!body.empty()) body += ", ";
                body += key_text(k) + " = ";
                write_inline(body, x, o);
            }
            out += body.empty() ? "{}" : "{ " + body + " }";
            break;
        }
    }
}

[[nodiscard]] inline bool is_table_array(const Value& v) {
    if (!v.is_array() || v.as_array().empty()) return false;
    for (const auto& x : v.as_array())
        if (!x.is_object()) return false;
    return true;
}

inline void write_table(std::string& out, const std::vector<std::string>& path, const Object& obj, const WriteOptions& o) {
    // 1) plain key/value pairs
    for (const auto& [k, v] : obj) {
        if (v.is_null()) {
            if (o.skip_null) continue;
            throw DataException("TOML: null cannot be represented (key '" + k + "')");
        }
        if (v.is_object() || is_table_array(v)) continue;
        out += key_text(k) + " = ";
        write_inline(out, v, o);
        out += '\n';
    }
    auto header_text = [&](const std::string& k) {
        std::string h;
        for (const auto& p : path) h += key_text(p) + ".";
        return h + key_text(k);
    };
    // 2) sub-tables and arrays of tables
    for (const auto& [k, v] : obj) {
        if (v.is_object()) {
            if (!out.empty()) out += '\n';
            out += "[" + header_text(k) + "]\n";
            auto p = path;
            p.push_back(k);
            write_table(out, p, v.as_object(), o);
        } else if (is_table_array(v)) {
            for (const auto& item : v.as_array()) {
                if (!out.empty()) out += '\n';
                out += "[[" + header_text(k) + "]]\n";
                auto p = path;
                p.push_back(k);
                write_table(out, p, item.as_object(), o);
            }
        }
    }
}
}  // namespace detail

/// Writes a TOML document. The root must be an object.
[[nodiscard]] inline std::string write(const Value& root, const WriteOptions& opt = {}) {
    if (!root.is_object()) throw DataException("TOML: the document root must be a table");
    std::string out;
    detail::write_table(out, {}, root.as_object(), opt);
    return out;
}

}  // namespace flyg::data::toml
