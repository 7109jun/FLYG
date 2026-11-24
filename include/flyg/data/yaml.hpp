// flyG - YAML 1.2 (practical subset): parser and emitter
// SPDX-License-Identifier: GPL-2.0-only
//
// Supported: block mappings/sequences (incl. compact "- key: v"), flow collections, plain /
// single-quoted / double-quoted scalars (multi-line folding, all escapes), block scalars (| and >
// with chomping and indentation indicators), comments, multiple documents (--- / ...), anchors,
// aliases, merge keys (<<), core-schema type resolution, and the tags !!str !!int !!float !!bool
// !!null !!binary (other tags are ignored).
// Not supported: complex keys ("? "), directives other than being skipped, custom tag handles,
// non-UTF-8 input. Alias expansion is limited to protect against "billion laughs" documents.
#pragma once

#include <charconv>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data::yaml {

struct ParseOptions {
    usize max_depth = 256;
    usize max_alias_nodes = 1'000'000;  ///< total nodes that aliases may expand to
};

namespace detail {

[[nodiscard]] inline bool is_flow_indicator(char c) noexcept { return c == ',' || c == '[' || c == ']' || c == '{' || c == '}'; }

/// Core schema resolution of a plain scalar.
[[nodiscard]] inline Value resolve_plain(const std::string& t) {
    if (t.empty() || t == "~" || t == "null" || t == "Null" || t == "NULL") return Value();
    if (t == "true" || t == "True" || t == "TRUE") return Value(true);
    if (t == "false" || t == "False" || t == "FALSE") return Value(false);
    const char* b = t.data();
    const char* e = b + t.size();
    // integers
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'o')) {
        u64 u = 0;
        const int base = t[1] == 'x' ? 16 : 8;
        if (auto r = std::from_chars(b + 2, e, u, base); r.ec == std::errc() && r.ptr == e && u <= static_cast<u64>(std::numeric_limits<i64>::max()))
            return Value(static_cast<i64>(u));
        return Value(t);
    }
    {
        const char* p = b;
        if (*p == '+' || *p == '-') ++p;
        bool digits = p < e;
        for (const char* q = p; q < e; ++q)
            if (*q < '0' || *q > '9') digits = false;
        if (digits) {
            i64 v = 0;
            const char* start = *b == '+' ? b + 1 : b;
            if (auto r = std::from_chars(start, e, v); r.ec == std::errc() && r.ptr == e) return Value(v);
            double d = 0;
            if (auto r = std::from_chars(start, e, d); r.ptr == e) return Value(d);  // too big for i64
            return Value(t);
        }
    }
    // floats
    if (t == ".inf" || t == ".Inf" || t == ".INF" || t == "+.inf" || t == "+.Inf" || t == "+.INF") return Value(std::numeric_limits<double>::infinity());
    if (t == "-.inf" || t == "-.Inf" || t == "-.INF") return Value(-std::numeric_limits<double>::infinity());
    if (t == ".nan" || t == ".NaN" || t == ".NAN") return Value(std::numeric_limits<double>::quiet_NaN());
    {
        // [-+]? ( \.[0-9]+ | [0-9]+ (\.[0-9]*)? ) ([eE][-+]?[0-9]+)?
        const char* p = b;
        if (*p == '+' || *p == '-') ++p;
        const char* q = p;
        usize int_digits = 0, frac_digits = 0;
        while (q < e && *q >= '0' && *q <= '9') ++q, ++int_digits;
        bool dot = false;
        if (q < e && *q == '.') {
            dot = true;
            ++q;
            while (q < e && *q >= '0' && *q <= '9') ++q, ++frac_digits;
        }
        bool ok = (int_digits > 0 || frac_digits > 0) && (dot || q < e);
        if (ok && int_digits == 0 && frac_digits == 0) ok = false;
        if (ok && q < e && (*q == 'e' || *q == 'E')) {
            ++q;
            if (q < e && (*q == '+' || *q == '-')) ++q;
            usize ed = 0;
            while (q < e && *q >= '0' && *q <= '9') ++q, ++ed;
            if (ed == 0) ok = false;
        }
        if (ok && q == e) {
            double d = 0;
            const char* start = *b == '+' ? b + 1 : b;
            if (auto r = std::from_chars(start, e, d); r.ptr == e && r.ec != std::errc::invalid_argument) return Value(d);
        }
    }
    return Value(t);
}

class Parser {
public:
    Parser(std::string_view s, const ParseOptions& o) : s_(s), o_(o) {}

    std::vector<Value> documents() {
        std::vector<Value> docs;
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
        bool explicit_start = false;
        for (;;) {
            skip_to_content();
            while (pos_ < s_.size() && at_line_start() && s_[pos_] == '%') {  // directives
                skip_line();
                skip_to_content();
            }
            if (is_marker("---")) {
                pos_ += 3;
                explicit_start = true;
            }
            if (pos_ >= s_.size() && !explicit_start) break;
            anchors_.clear();
            alias_nodes_ = 0;
            skip_inline_ws_and_comment();
            Value v;
            if (peek_is_eol()) {
                skip_to_content();
                if (pos_ < s_.size() && !is_marker("---") && !is_marker("...")) v = node(-1, false, 0);
            } else {
                v = node(-1, false, 0);
            }
            docs.push_back(std::move(v));
            skip_to_content();
            if (is_marker("...")) {
                pos_ += 3;
                skip_inline_ws_and_comment();
                skip_to_content();
                explicit_start = false;
                if (pos_ >= s_.size()) break;
                continue;
            }
            if (pos_ >= s_.size()) break;
            if (!is_marker("---")) fail("unexpected content after the document");
            explicit_start = false;
        }
        if (docs.empty()) docs.emplace_back();
        return docs;
    }

private:
    // ---- low level helpers ---------------------------------------------------------------------
    [[noreturn]] void fail(const std::string& what, usize at = static_cast<usize>(-1)) const {
        const usize p = at == static_cast<usize>(-1) ? pos_ : at;
        usize line = 1, col = 1;
        for (usize i = 0; i < p && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw DataException("YAML: " + what + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")", p);
    }
    [[nodiscard]] char cur() const noexcept { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    [[nodiscard]] char at(usize i) const noexcept { return i < s_.size() ? s_[i] : '\0'; }
    [[nodiscard]] usize line_start(usize p) const noexcept {
        while (p > 0 && s_[p - 1] != '\n') --p;
        return p;
    }
    [[nodiscard]] usize column(usize p) const noexcept { return p - line_start(p); }
    [[nodiscard]] bool at_line_start() const noexcept { return pos_ == 0 || s_[pos_ - 1] == '\n'; }
    [[nodiscard]] bool is_space_or_eol(char c) const noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0'; }
    [[nodiscard]] bool peek_is_eol() const noexcept {
        usize p = pos_;
        while (at(p) == ' ' || at(p) == '\t') ++p;
        return p >= s_.size() || s_[p] == '\n' || s_[p] == '\r' || s_[p] == '#';
    }
    [[nodiscard]] bool is_marker(std::string_view m) const noexcept {
        return at_line_start() && s_.compare(pos_, 3, m) == 0 && is_space_or_eol(at(pos_ + 3));
    }
    void skip_line() {
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
        if (pos_ < s_.size()) ++pos_;
    }
    void skip_inline_ws_and_comment() {
        while (cur() == ' ' || cur() == '\t') ++pos_;
        if (cur() == '#' && (pos_ == 0 || is_space_or_eol(s_[pos_ - 1]))) {
            while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
        }
    }
    /// Moves to the next token: skips blanks, comments and line breaks. Rejects tab indentation.
    void skip_to_content() {
        for (;;) {
            while (cur() == ' ') ++pos_;
            if (cur() == '\t') {
                bool only_spaces_before = true;  // is this tab part of the line's indentation?
                for (usize i = line_start(pos_); i < pos_; ++i)
                    if (s_[i] != ' ') only_spaces_before = false;
                usize q = pos_;
                while (at(q) == ' ' || at(q) == '\t') ++q;
                if (only_spaces_before && q < s_.size() && at(q) != '\n' && at(q) != '\r' && at(q) != '#')
                    fail("tabs are not allowed for indentation");
                pos_ = q;
            }
            if (cur() == '#') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            }
            if (cur() == '\r' || cur() == '\n') {
                if (cur() == '\r' && at(pos_ + 1) == '\n') ++pos_;
                ++pos_;
                continue;
            }
            return;
        }
    }

    // ---- values --------------------------------------------------------------------------------
    struct Props {
        bool has_anchor = false;
        std::string anchor;
        std::string tag;
    };

    Props properties() {
        Props p;
        for (;;) {
            if (cur() == '&') {
                if (p.has_anchor) fail("duplicate anchor");
                ++pos_;
                const usize b = pos_;
                while (pos_ < s_.size() && !is_space_or_eol(s_[pos_]) && !is_flow_indicator(s_[pos_])) ++pos_;
                if (pos_ == b) fail("empty anchor name");
                p.has_anchor = true;
                p.anchor = std::string(s_.substr(b, pos_ - b));
            } else if (cur() == '!') {
                if (!p.tag.empty()) fail("duplicate tag");
                const usize b = pos_++;
                while (pos_ < s_.size() && !is_space_or_eol(s_[pos_]) && !is_flow_indicator(s_[pos_])) ++pos_;
                p.tag = std::string(s_.substr(b, pos_ - b));
            } else {
                return p;
            }
            while (cur() == ' ' || cur() == '\t') ++pos_;
        }
    }

    Value apply_tag(const std::string& tag, Value v, bool was_plain_scalar, const std::string& raw) {
        if (tag.empty() || tag == "!") return v;
        if ((v.is_array() || v.is_object()) && (tag == "!!str" || tag == "!!int" || tag == "!!float" || tag == "!!bool" || tag == "!!null" || tag == "!!binary"))
            fail("tag " + tag + " cannot be applied to a collection");
        if (tag == "!!str") return Value(was_plain_scalar ? raw : (v.is_string() ? v.as_string() : raw));
        if (tag == "!!null") return Value();
        if (tag == "!!bool") {
            Value r = resolve_plain(raw);
            if (!r.is_bool()) fail("value is not a !!bool");
            return r;
        }
        if (tag == "!!int") {
            Value r = resolve_plain(raw);
            if (!r.is_integer()) fail("value is not an !!int");
            return r;
        }
        if (tag == "!!float") {
            Value r = resolve_plain(raw);
            if (!r.is_number()) fail("value is not a !!float");
            return Value(r.as_double());
        }
        if (tag == "!!binary") {
            if (!v.is_string()) fail("!!binary needs a string value");
            return Value(base64_decode(v.as_string()));
        }
        return v;  // unknown tags are ignored
    }

    void register_anchor(const Props& p, const Value& v) {
        if (p.has_anchor) anchors_[p.anchor] = v;
    }

    static usize count_nodes(const Value& v) {
        usize n = 1;
        if (v.is_array()) for (const auto& x : v.as_array()) n += count_nodes(x);
        else if (v.is_object()) for (const auto& e : v.as_object()) n += 1 + count_nodes(e.second);
        return n;
    }

    Value alias() {
        ++pos_;  // '*'
        const usize b = pos_;
        while (pos_ < s_.size() && !is_space_or_eol(s_[pos_]) && !is_flow_indicator(s_[pos_])) ++pos_;
        const std::string name(s_.substr(b, pos_ - b));
        const auto it = anchors_.find(name);
        if (it == anchors_.end()) fail("undefined alias '*" + name + "'", b);
        alias_nodes_ += count_nodes(it->second);
        if (alias_nodes_ > o_.max_alias_nodes) fail("alias expansion limit exceeded");
        return it->second;
    }

    /// Parses a node whose first token is at `pos_`. `parent` is the indentation of the enclosing
    /// block collection (-1 at top level); a block collection started here uses the token's column.
    /// `inline_ctx` forbids starting a block collection on the same line as its parent key.
    Value node(int parent, bool inline_ctx, usize depth) {
        if (depth > o_.max_depth) fail("maximum nesting depth exceeded");
        const usize start_col = column(pos_);
        Props props = properties();
        if (props.has_anchor || !props.tag.empty()) {
            // properties may be followed by a line break and then the node
            if (peek_is_eol()) {
                skip_to_content();
                if (pos_ >= s_.size() || is_marker("---") || is_marker("...") || static_cast<int>(column(pos_)) <= parent) {
                    Value v = apply_tag(props.tag, Value(), true, "");
                    register_anchor(props, v);
                    return v;
                }
                Value v = node_body(parent, false, depth, props, column(pos_));
                return v;
            }
        }
        return node_body(parent, inline_ctx, depth, props, start_col);
    }

    Value node_body(int parent, bool inline_ctx, usize depth, const Props& props, usize col) {
        const char c = cur();
        const int icol = static_cast<int>(col);
        // block sequence
        if (c == '-' && is_space_or_eol(at(pos_ + 1))) {
            if (inline_ctx) fail("a block sequence cannot start on the same line as its key");
            Value v = block_sequence(icol, depth);
            register_anchor(props, v);
            return apply_tag(props.tag, std::move(v), false, "");
        }
        if (c == '?' && is_space_or_eol(at(pos_ + 1))) fail("complex mapping keys ('? ') are not supported");
        if (c == '*') {
            Value a = alias();
            skip_blanks();
            if (cur() == ':' && is_space_or_eol(at(pos_ + 1))) {
                if (inline_ctx) fail("a block mapping cannot start on the same line as its key");
                Value v = block_mapping(icol, std::move(a), depth, props);
                return v;
            }
            return a;
        }
        if (c == '|' || c == '>') {
            Value v = Value(block_scalar(parent));
            register_anchor(props, v);
            return apply_tag(props.tag, std::move(v), false, v.as_string());
        }
        // flow collections (possibly a key of a block mapping when followed by ':')
        if (c == '[' || c == '{') {
            Value v = flow_node(depth);
            skip_blanks();
            if (cur() == ':' && is_space_or_eol(at(pos_ + 1))) {
                if (inline_ctx) fail("a block mapping cannot start on the same line as its key");
                return block_mapping(icol, std::move(v), depth, props);
            }
            v = apply_tag(props.tag, std::move(v), false, "");
            register_anchor(props, v);
            return v;
        }
        // scalar or mapping key
        const usize scalar_start = pos_;
        bool quoted = false;
        std::string text;
        if (c == '"') { text = double_quoted(); quoted = true; }
        else if (c == '\'') { text = single_quoted(); quoted = true; }
        else {
            text = plain_line(false);
        }
        skip_blanks();
        if (cur() == ':' && (is_space_or_eol(at(pos_ + 1)))) {
            if (inline_ctx) fail("a block mapping cannot start on the same line as its key", scalar_start);
            Value key = quoted ? Value(text) : resolve_key(text, props);
            return block_mapping(icol, std::move(key), depth, props);
        }
        Value v;
        if (!quoted) {
            // continuation lines of a multi-line plain scalar
            text = plain_continue(std::move(text), parent);
            v = props.tag.empty() ? resolve_plain(text) : Value(text);
            v = apply_tag(props.tag, std::move(v), true, text);
        } else {
            v = apply_tag(props.tag, Value(text), false, text);
        }
        register_anchor(props, v);
        return v;
    }

    Value resolve_key(const std::string& text, const Props&) { return resolve_plain(text); }

    void skip_blanks() {
        while (cur() == ' ' || cur() == '\t') ++pos_;
    }

    // Plain scalar, single line, in block context: ends at ": ", " #", or end of line.
    std::string plain_line(bool in_flow) {
        const char first = cur();
        if (first == '\0') return {};
        if (first == '@' || first == '`') fail("reserved indicator at the start of a plain scalar");
        if ((first == '-' || first == '?' || first == ':') && is_space_or_eol(at(pos_ + 1))) fail("unexpected indicator");
        std::string out;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '\n' || c == '\r') break;
            if (c == ':' && (is_space_or_eol(at(pos_ + 1)) || (in_flow && is_flow_indicator(at(pos_ + 1))))) break;
            if (c == '#' && !out.empty() && (out.back() == ' ' || out.back() == '\t')) break;
            if (in_flow && is_flow_indicator(c)) break;
            out += c;
            ++pos_;
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        return out;
    }

    // After the first line of a plain scalar: fold following more-indented lines into it.
    std::string plain_continue(std::string text, int parent) {
        for (;;) {
            usize p = pos_;
            while (at(p) == ' ' || at(p) == '\t') ++p;
            if (at(p) == '#' ) break;  // comment ends the scalar
            if (at(p) != '\n' && at(p) != '\r') break;  // something else on the same line (e.g. ':')
            // count blank lines, find next content line
            usize q = p;
            int blanks = 0;
            for (;;) {
                if (at(q) == '\r') ++q;
                if (at(q) != '\n') break;
                ++q;
                usize r = q;
                while (at(r) == ' ') ++r;
                if (at(r) == '\n' || at(r) == '\r') { ++blanks; q = r; continue; }
                q = r;
                break;
            }
            if (q >= s_.size()) break;
            const usize ls = line_start(q);
            const int ind = static_cast<int>(q - ls);
            if (ind <= parent) break;
            if (at(q) == '#') break;
            if ((s_.compare(ls, 3, "---") == 0 || s_.compare(ls, 3, "...") == 0) && is_space_or_eol(at(ls + 3))) break;
            // a line that looks like "key: value" or a sequence entry is not a continuation
            if (at(q) == '-' && is_space_or_eol(at(q + 1)) && parent >= -1 && ind <= parent + 0) break;
            const usize save = pos_;
            pos_ = q;
            std::string next = plain_line(false);
            if (cur() == ':' && is_space_or_eol(at(pos_ + 1))) { pos_ = save; break; }
            if (next.empty()) { pos_ = save; break; }
            text += blanks > 0 ? std::string(static_cast<usize>(blanks), '\n') : std::string(" ");
            text += next;
        }
        return text;
    }

    std::string single_quoted() {
        ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated single-quoted scalar");
            const char c = s_[pos_];
            if (c == '\'') {
                if (at(pos_ + 1) == '\'') { out += '\''; pos_ += 2; continue; }
                ++pos_;
                return out;
            }
            if (c == '\n' || c == '\r') { fold_quoted_break(out); continue; }
            out += c;
            ++pos_;
        }
    }

    // Folds a line break inside a quoted scalar: trailing blanks removed, one break -> space,
    // n>1 breaks -> n-1 newlines, leading blanks of the next line removed.
    void fold_quoted_break(std::string& out) {
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        int breaks = 0;
        while (pos_ < s_.size()) {
            if (cur() == '\r') ++pos_;
            if (cur() == '\n') {
                ++breaks;
                ++pos_;
                while (cur() == ' ' || cur() == '\t') ++pos_;
                continue;
            }
            break;
        }
        if (breaks <= 1) out += ' ';
        else out.append(static_cast<usize>(breaks - 1), '\n');
    }

    std::string double_quoted() {
        ++pos_;
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated double-quoted scalar");
            const char c = s_[pos_];
            if (c == '"') { ++pos_; return out; }
            if (c == '\n' || c == '\r') { fold_quoted_break(out); continue; }
            if (c != '\\') { out += c; ++pos_; continue; }
            ++pos_;
            const char e = cur();
            ++pos_;
            auto hex = [&](int n) {
                char32_t v = 0;
                for (int i = 0; i < n; ++i) {
                    const char h = cur();
                    int d;
                    if (h >= '0' && h <= '9') d = h - '0';
                    else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                    else fail("invalid hexadecimal escape");
                    v = (v << 4) | static_cast<char32_t>(d);
                    ++pos_;
                }
                return v;
            };
            switch (e) {
                case '0': out += '\0'; break;
                case 'a': out += '\a'; break;
                case 'b': out += '\b'; break;
                case 't': case '\t': out += '\t'; break;
                case 'n': out += '\n'; break;
                case 'v': out += '\v'; break;
                case 'f': out += '\f'; break;
                case 'r': out += '\r'; break;
                case 'e': out += '\x1b'; break;
                case ' ': out += ' '; break;
                case '"': out += '"'; break;
                case '/': out += '/'; break;
                case '\\': out += '\\'; break;
                case 'N': utf8::append(out, 0x85); break;
                case '_': utf8::append(out, 0xA0); break;
                case 'L': utf8::append(out, 0x2028); break;
                case 'P': utf8::append(out, 0x2029); break;
                case 'x': { const char32_t cp = hex(2); utf8::append(out, cp); break; }
                case 'u': {
                    char32_t cp = hex(4);
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (cur() == '\\' && at(pos_ + 1) == 'u') {
                            pos_ += 2;
                            const char32_t lo = hex(4);
                            if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            fail("unpaired surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    utf8::append(out, cp);
                    break;
                }
                case 'U': {
                    const char32_t cp = hex(8);
                    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) fail("invalid code point");
                    utf8::append(out, cp);
                    break;
                }
                case '\n':
                case '\r':  // escaped line break: joins lines without a space
                    if (e == '\r' && cur() == '\n') ++pos_;
                    while (cur() == ' ' || cur() == '\t') ++pos_;
                    break;
                default: --pos_; fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }

    // ---- block scalars -------------------------------------------------------------------------
    std::string block_scalar(int parent) {
        const bool literal = cur() == '|';
        ++pos_;
        int explicit_indent = 0;
        char chomp = 'c';  // clip
        for (int i = 0; i < 2; ++i) {
            if (cur() == '+' || cur() == '-') chomp = cur(), ++pos_;
            else if (cur() >= '1' && cur() <= '9' && explicit_indent == 0) explicit_indent = cur() - '0', ++pos_;
        }
        skip_inline_ws_and_comment();
        if (pos_ < s_.size() && cur() != '\n' && cur() != '\r') fail("unexpected text after the block scalar header");
        // gather lines
        std::vector<std::string> lines;  // content with indentation removed; blank lines are ""
        std::vector<bool> blank;
        int block_indent = explicit_indent > 0 ? std::max(parent, 0) + explicit_indent : -1;
        if (explicit_indent > 0 && parent < 0) block_indent = explicit_indent;
        if (cur() == '\r') ++pos_;
        if (cur() == '\n') ++pos_;
        while (pos_ < s_.size()) {
            const usize ls = pos_;
            usize p = ls;
            while (at(p) == ' ') ++p;
            const int ind = static_cast<int>(p - ls);
            const bool is_blank = at(p) == '\n' || at(p) == '\r' || p >= s_.size();
            if (is_blank) {
                // blank (or whitespace-only) line
                std::string extra;
                if (block_indent >= 0 && ind > block_indent) extra = std::string(static_cast<usize>(ind - block_indent), ' ');
                while (at(p) != '\n' && p < s_.size()) ++p;
                lines.push_back(extra);
                blank.push_back(extra.empty());
                pos_ = p < s_.size() ? p + 1 : p;
                if (p >= s_.size()) break;
                continue;
            }
            if (block_indent < 0) {
                if (ind <= parent) break;
                block_indent = ind;
            }
            if (ind < block_indent) break;
            if (ind == 0 && ((s_.compare(ls, 3, "---") == 0 || s_.compare(ls, 3, "...") == 0) && is_space_or_eol(at(ls + 3)))) break;
            usize e = p;
            while (e < s_.size() && s_[e] != '\n') ++e;
            std::string content(s_.substr(ls + static_cast<usize>(block_indent), e - ls - static_cast<usize>(block_indent)));
            if (!content.empty() && content.back() == '\r') content.pop_back();
            lines.push_back(std::move(content));
            blank.push_back(false);
            pos_ = e < s_.size() ? e + 1 : e;
        }
        // trailing blank lines are subject to chomping
        usize trailing = 0;
        while (!lines.empty() && blank.back()) {
            lines.pop_back();
            blank.pop_back();
            ++trailing;
        }
        std::string out;
        if (literal) {
            for (usize i = 0; i < lines.size(); ++i) {
                out += lines[i];
                if (i + 1 < lines.size()) out += '\n';
            }
        } else {
            // folded: line breaks between two non-indented, non-blank lines become spaces
            for (usize i = 0; i < lines.size(); ++i) {
                out += lines[i];
                if (i + 1 == lines.size()) break;
                const bool cur_more = !lines[i].empty() && (lines[i][0] == ' ' || lines[i][0] == '\t');
                const bool next_more = !lines[i + 1].empty() && (lines[i + 1][0] == ' ' || lines[i + 1][0] == '\t');
                if (blank[i] ) { out += '\n'; continue; }
                if (blank[i + 1]) {
                    // run of blank lines: n blanks -> n newlines (the break before them is folded away)
                    usize j = i + 1;
                    usize n = 0;
                    while (j < lines.size() && blank[j]) ++j, ++n;
                    if (j >= lines.size()) { out += '\n'; continue; }
                    out.append(n, '\n');
                    if (cur_more || (!lines[j].empty() && (lines[j][0] == ' ' || lines[j][0] == '\t'))) out += '\n';
                    // skip the blank lines we consumed
                    for (usize k = i + 1; k < j; ++k) lines[k].clear();
                    i = j - 1;
                    continue;
                }
                out += (cur_more || next_more) ? '\n' : ' ';
            }
        }
        if (!lines.empty()) {
            if (chomp == 'c') out += '\n';
            else if (chomp == '+') out += std::string(1 + trailing, '\n');
        } else if (chomp == '+') {
            out += std::string(trailing, '\n');
        }
        return out;
    }

    // ---- block collections ---------------------------------------------------------------------
    Value block_sequence(int indent, usize depth) {
        Array items;
        for (;;) {
            // at '-' with column == indent
            ++pos_;  // '-'
            skip_blanks();
            if (peek_is_eol()) {
                skip_to_content();
                if (pos_ >= s_.size() || is_marker("---") || is_marker("...") || static_cast<int>(column(pos_)) <= indent) {
                    items.emplace_back();
                } else {
                    items.push_back(node(indent, false, depth + 1));
                }
            } else {
                items.push_back(node(indent, false, depth + 1));
            }
            skip_to_content();
            if (pos_ >= s_.size() || is_marker("---") || is_marker("...")) break;
            const int c = static_cast<int>(column(pos_));
            if (c < indent) break;
            if (c > indent) fail("bad indentation of a sequence entry");
            if (!(cur() == '-' && is_space_or_eol(at(pos_ + 1)))) break;  // belongs to the parent mapping
        }
        return Value(std::move(items));
    }

    Value block_mapping(int indent, Value first_key, usize depth, const Props& props) {
        Object obj;
        std::set<std::string> merged;
        Value key = std::move(first_key);
        const Props map_props = props;  // anchor/tag of the mapping itself
        for (;;) {
            // at ':' after the key
            if (cur() != ':') fail("expected ':'");
            ++pos_;
            skip_blanks();
            Value val;
            const bool key_is_merge = key.is_string() && key.as_string() == "<<";
            if (peek_is_eol()) {
                skip_to_content();
                const bool seq_at_same_indent = pos_ < s_.size() && cur() == '-' && is_space_or_eol(at(pos_ + 1)) && static_cast<int>(column(pos_)) == indent;
                if (pos_ >= s_.size() || is_marker("---") || is_marker("...") || (static_cast<int>(column(pos_)) <= indent && !seq_at_same_indent)) {
                    val = Value();
                } else if (seq_at_same_indent) {
                    val = block_sequence(indent, depth + 1);
                } else {
                    val = node(indent, false, depth + 1);
                }
            } else {
                val = node(indent, true, depth + 1);
            }
            if (key_is_merge) merge_into(obj, val, merged);
            else insert_key(obj, key, std::move(val), &merged);
            skip_to_content();
            if (pos_ >= s_.size() || is_marker("---") || is_marker("...")) break;
            const int c = static_cast<int>(column(pos_));
            if (c < indent) break;
            if (c > indent) fail("bad indentation of a mapping entry");
            if (cur() == '-' && is_space_or_eol(at(pos_ + 1))) break;
            // next key
            const char k = cur();
            std::string kt;
            bool quoted = false;
            if (k == '"') { kt = double_quoted(); quoted = true; }
            else if (k == '\'') { kt = single_quoted(); quoted = true; }
            else if (k == '*') { key = alias(); skip_blanks(); continue; }
            else if (k == '[' || k == '{') { key = flow_node(depth + 1); skip_blanks(); continue; }
            else if (k == '?' && is_space_or_eol(at(pos_ + 1))) fail("complex mapping keys ('? ') are not supported");
            else {
                (void)properties();  // properties on non-first keys are accepted and ignored
                kt = plain_line(false);
            }
            skip_blanks();
            if (!(cur() == ':' && is_space_or_eol(at(pos_ + 1)))) fail("expected a mapping key followed by ':'");
            key = quoted ? Value(kt) : resolve_plain(kt);
        }
        Value v(std::move(obj));
        register_anchor(map_props, v);
        return apply_tag(map_props.tag, std::move(v), false, "");
    }

    void insert_key(Object& obj, const Value& key, Value val, std::set<std::string>* merged = nullptr) {
        std::string ks;
        if (key.is_string()) ks = key.as_string();
        else if (key.is_null()) ks = "null";
        else if (key.is_bool()) ks = key.as_bool() ? "true" : "false";
        else if (key.is_integer()) ks = std::to_string(key.as_int());
        else if (key.is_real()) {
            char buf[40];
            const auto r = std::to_chars(buf, buf + sizeof buf, key.as_double());
            ks.assign(buf, r.ptr);
        } else fail("only scalar mapping keys are supported");
        if (obj.contains(ks)) {
            if (merged && merged->erase(ks) > 0) {  // explicit keys override keys that came from '<<'
                obj.insert_or_assign(std::move(ks), std::move(val));
                return;
            }
            fail("duplicate mapping key '" + ks + "'");
        }
        obj.append_unchecked(std::move(ks), std::move(val));
    }

    void merge_into(Object& obj, const Value& src, std::set<std::string>& merged) {
        auto merge_one = [&](const Value& m) {
            if (!m.is_object()) fail("merge key '<<' needs a mapping (or a list of mappings)");
            for (const auto& [k, v] : m.as_object())
                if (!obj.contains(k)) {
                    obj.append_unchecked(k, v);
                    merged.insert(k);
                }
        };
        if (src.is_array()) for (const auto& m : src.as_array()) merge_one(m);
        else merge_one(src);
    }

    // ---- flow collections ----------------------------------------------------------------------
    void flow_ws() {
        for (;;) {
            while (cur() == ' ' || cur() == '\t' || cur() == '\n' || cur() == '\r') ++pos_;
            if (cur() == '#' && (pos_ == 0 || is_space_or_eol(s_[pos_ - 1]))) {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            return;
        }
    }

    Value flow_node(usize depth) {
        if (depth > o_.max_depth) fail("maximum nesting depth exceeded");
        flow_ws();
        Props props = properties();
        flow_ws();
        const char c = cur();
        Value v;
        if (c == '[') {
            ++pos_;
            Array items;
            for (;;) {
                flow_ws();
                if (cur() == ']') { ++pos_; break; }
                if (pos_ >= s_.size()) fail("unterminated flow sequence");
                Value item = flow_node(depth + 1);
                flow_ws();
                if (cur() == ':' && (is_space_or_eol(at(pos_ + 1)) || is_flow_indicator(at(pos_ + 1)))) {  // single-pair mapping
                    ++pos_;
                    flow_ws();
                    Value val = (cur() == ',' || cur() == ']') ? Value() : flow_node(depth + 1);
                    Object o;
                    insert_key(o, item, std::move(val));
                    item = Value(std::move(o));
                    flow_ws();
                }
                items.push_back(std::move(item));
                if (cur() == ',') { ++pos_; continue; }
                if (cur() == ']') { ++pos_; break; }
                fail("expected ',' or ']' in flow sequence");
            }
            v = Value(std::move(items));
        } else if (c == '{') {
            ++pos_;
            Object obj;
            std::set<std::string> merged;
            for (;;) {
                flow_ws();
                if (cur() == '}') { ++pos_; break; }
                if (pos_ >= s_.size()) fail("unterminated flow mapping");
                Value key = flow_node(depth + 1);
                flow_ws();
                Value val;
                if (cur() == ':' && (is_space_or_eol(at(pos_ + 1)) || is_flow_indicator(at(pos_ + 1)) || key.is_string())) {
                    ++pos_;
                    flow_ws();
                    val = (cur() == ',' || cur() == '}') ? Value() : flow_node(depth + 1);
                }
                const bool merge = key.is_string() && key.as_string() == "<<";
                if (merge) merge_into(obj, val, merged);
                else insert_key(obj, key, std::move(val), &merged);
                flow_ws();
                if (cur() == ',') { ++pos_; continue; }
                if (cur() == '}') { ++pos_; break; }
                fail("expected ',' or '}' in flow mapping");
            }
            v = Value(std::move(obj));
        } else if (c == '*') {
            v = alias();
        } else if (c == '"') {
            const std::string t = double_quoted();
            v = apply_tag(props.tag, Value(t), false, t);
        } else if (c == '\'') {
            const std::string t = single_quoted();
            v = apply_tag(props.tag, Value(t), false, t);
        } else if (c == ',' || c == ']' || c == '}' || c == ':') {
            v = apply_tag(props.tag, Value(), true, "");
        } else {
            std::string t = plain_line(true);
            // multi-line plain scalars inside flow collections
            for (;;) {
                usize p = pos_;
                while (at(p) == ' ' || at(p) == '\t') ++p;
                if (at(p) != '\n' && at(p) != '\r') break;
                const usize save = pos_;
                pos_ = p;
                flow_ws();
                if (pos_ >= s_.size() || is_flow_indicator(cur()) || cur() == ':' ) { pos_ = save; break; }
                if (cur() == '"' || cur() == '\'' || cur() == '&' || cur() == '*' || cur() == '!') { pos_ = save; break; }
                const std::string next = plain_line(true);
                if (next.empty()) { pos_ = save; break; }
                t += ' ';
                t += next;
            }
            v = props.tag.empty() ? resolve_plain(t) : apply_tag(props.tag, Value(t), true, t);
        }
        register_anchor(props, v);
        if (c == '[' || c == '{') return apply_tag(props.tag, std::move(v), false, "");
        return v;
    }

    std::string_view s_;
    const ParseOptions& o_;
    usize pos_ = 0;
    std::map<std::string, Value> anchors_;
    usize alias_nodes_ = 0;
};

}  // namespace detail

/// Parses a single-document stream. Throws if it contains more than one document.
[[nodiscard]] inline Value parse(std::string_view text, const ParseOptions& opt = {}) {
    if (!utf8::is_valid(text)) throw DataException("YAML: input is not valid UTF-8");
    auto docs = detail::Parser(text, opt).documents();
    if (docs.size() != 1) throw DataException("YAML: expected a single document but found " + std::to_string(docs.size()) + " (use parse_all)");
    return std::move(docs[0]);
}

[[nodiscard]] inline std::vector<Value> parse_all(std::string_view text, const ParseOptions& opt = {}) {
    if (!utf8::is_valid(text)) throw DataException("YAML: input is not valid UTF-8");
    return detail::Parser(text, opt).documents();
}

// ---------------------------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------------------------
struct EmitOptions {
    int indent = 2;                  ///< spaces per level (1..8)
    bool document_marker = false;    ///< prefix with "---"
    bool sort_keys = false;
};

namespace detail {

[[nodiscard]] inline bool needs_quotes(const std::string& s) {
    if (s.empty()) return true;
    if (resolve_plain(s).type() != Type::string) return true;  // would read back as null/bool/number
    const char f = s.front();
    if (f == ' ' || s.back() == ' ' || f == '\t' || s.back() == '\t') return true;
    if (std::string_view("-?:,[]{}#&*!|>'\"%@`").find(f) != std::string_view::npos) {
        // "-x", "?x", ":x" are fine as plain scalars, but be conservative
        return true;
    }
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return true;
        if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' ')) return true;
        if (c == '#' && i > 0 && s[i - 1] == ' ') return true;
    }
    if (s == "---" || s == "..." || s.rfind("--- ", 0) == 0) return true;
    return false;
}

inline void emit_double_quoted(std::string& out, const std::string& s) {
    out += '"';
    for (usize i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case 0: out += "\\0"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    static const char* hx = "0123456789abcdef";
                    out += "\\x";
                    out += hx[c >> 4];
                    out += hx[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

inline void emit_scalar_string(std::string& out, const std::string& s) {
    if (!utf8::is_valid(s)) throw DataException("YAML: cannot emit a string that is not valid UTF-8");
    if (needs_quotes(s)) emit_double_quoted(out, s);
    else out += s;
}

inline void emit_double(std::string& out, double d) {
    if (std::isnan(d)) { out += ".nan"; return; }
    if (std::isinf(d)) { out += d < 0 ? "-.inf" : ".inf"; return; }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, d);
    std::string_view sv(buf, static_cast<usize>(r.ptr - buf));
    out += sv;
    if (sv.find_first_of(".eE") == std::string_view::npos) out += ".0";
}

inline void emit_node(std::string& out, const Value& v, const EmitOptions& o, int level, bool after_key_or_dash);

inline void emit_inline_scalar(std::string& out, const Value& v) {
    switch (v.type()) {
        case Type::null: out += "null"; break;
        case Type::boolean: out += v.as_bool() ? "true" : "false"; break;
        case Type::integer: out += std::to_string(v.as_int()); break;
        case Type::real: emit_double(out, v.as_double()); break;
        case Type::string: emit_scalar_string(out, v.as_string()); break;
        case Type::binary: out += "!!binary " + base64_encode(v.as_binary()); break;
        default: break;
    }
}

inline void emit_node(std::string& out, const Value& v, const EmitOptions& o, int level, bool inline_start) {
    const std::string pad(static_cast<usize>(level) * static_cast<usize>(o.indent), ' ');
    if (v.is_array()) {
        const auto& a = v.as_array();
        if (a.empty()) { out += "[]\n"; return; }
        bool first = true;
        for (const auto& item : a) {
            if (!(first && inline_start)) out += pad;
            first = false;
            out += '-';
            if ((item.is_array() && !item.as_array().empty()) || (item.is_object() && !item.as_object().empty())) {
                if (item.is_object() || item.is_array()) {
                    out += ' ';
                    emit_node(out, item, o, level + 1, true);
                }
            } else {
                out += ' ';
                if (item.is_array()) out += "[]\n";
                else if (item.is_object()) out += "{}\n";
                else { emit_inline_scalar(out, item); out += '\n'; }
            }
        }
    } else if (v.is_object()) {
        const auto& obj = v.as_object();
        if (obj.empty()) { out += "{}\n"; return; }
        std::vector<const Object::Entry*> order;
        for (const auto& e : obj) order.push_back(&e);
        if (o.sort_keys) std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->first < b->first; });
        bool first = true;
        for (const auto* e : order) {
            if (!(first && inline_start)) out += pad;
            first = false;
            emit_scalar_string(out, e->first);
            out += ':';
            const Value& x = e->second;
            if ((x.is_array() && !x.as_array().empty()) || (x.is_object() && !x.as_object().empty())) {
                out += '\n';
                emit_node(out, x, o, x.is_array() ? level : level + 1, false);
            } else {
                out += ' ';
                if (x.is_array()) out += "[]\n";
                else if (x.is_object()) out += "{}\n";
                else { emit_inline_scalar(out, x); out += '\n'; }
            }
        }
    } else {
        emit_inline_scalar(out, v);
        out += '\n';
    }
}
}  // namespace detail

[[nodiscard]] inline std::string emit(const Value& v, const EmitOptions& opt = {}) {
    if (opt.indent < 1 || opt.indent > 8) throw DataException("YAML: indent must be in 1..8");
    std::string out;
    if (opt.document_marker) out += "---\n";
    // A top-level sequence nested in a mapping value uses the same indent as its key (compact form);
    // here the document root starts at level 0.
    detail::emit_node(out, v, opt, 0, false);
    return out;
}

}  // namespace flyg::data::yaml
