// flyG - JSON (RFC 8259): parser, serializer, SAX events, incremental writer, NDJSON,
//        JSON Pointer (RFC 6901) and Merge Patch (RFC 7386)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <charconv>
#include <cmath>
#include <functional>
#include <iosfwd>
#include <limits>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data::json {

struct ParseOptions {
    bool allow_comments = false;         ///< accept // and /* */ comments
    bool allow_trailing_commas = false;  ///< accept [1,2,] and {"a":1,}
    bool allow_nan_inf = false;          ///< accept NaN, Infinity, -Infinity
    bool reject_duplicate_keys = false;  ///< throw instead of "last one wins"
    usize max_depth = 512;               ///< nesting limit (protects the stack)
};

struct WriteOptions {
    int indent = -1;                 ///< < 0: compact; >= 0: pretty with that many spaces per level
    bool sort_keys = false;
    bool ascii_only = false;         ///< escape every non-ASCII character as \uXXXX
    bool allow_nan_inf = false;      ///< write NaN/Infinity instead of throwing
    bool binary_as_base64 = true;    ///< binary values become base64 strings (else: throw)
};

/// Receives parse events in document order. Every callback has an empty default.
class Handler {
public:
    virtual ~Handler() = default;
    virtual void null_value() {}
    virtual void bool_value(bool) {}
    virtual void integer_value(i64) {}
    virtual void real_value(double) {}
    virtual void string_value(std::string_view) {}
    virtual void start_object() {}
    virtual void key(std::string_view) {}
    virtual void end_object() {}
    virtual void start_array() {}
    virtual void end_array() {}
};

namespace detail {

class Parser {
public:
    Parser(std::string_view text, Handler& h, const ParseOptions& o) : s_(text), h_(h), o_(o) {}

    void parse_document() {
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;  // UTF-8 BOM
        skip_ws();
        value(0);
        skip_ws();
        if (pos_ != s_.size()) fail("unexpected trailing characters");
    }

    /// Parses one value from the current position and returns the number of bytes consumed.
    usize parse_prefix() {
        skip_ws();
        value(0);
        return pos_;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        usize line = 1, col = 1;
        for (usize i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw DataException("JSON: " + what + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")", pos_);
    }

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= s_.size(); }
    [[nodiscard]] char peek() const noexcept { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    void skip_ws() {
        for (;;) {
            while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
            if (o_.allow_comments && pos_ + 1 < s_.size() && s_[pos_] == '/') {
                if (s_[pos_ + 1] == '/') {
                    while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                    continue;
                }
                if (s_[pos_ + 1] == '*') {
                    const auto end = s_.find("*/", pos_ + 2);
                    if (end == std::string_view::npos) { pos_ = s_.size(); fail("unterminated comment"); }
                    pos_ = end + 2;
                    continue;
                }
            }
            break;
        }
    }

    void expect_literal(std::string_view lit) {
        if (s_.compare(pos_, lit.size(), lit) != 0) fail("invalid literal");
        pos_ += lit.size();
    }

    void value(usize depth) {
        if (at_end()) fail("unexpected end of input");
        switch (peek()) {
            case '{': object(depth); return;
            case '[': array(depth); return;
            case '"': { std::string tmp; string_into(tmp); h_.string_value(tmp); return; }
            case 't': expect_literal("true"); h_.bool_value(true); return;
            case 'f': expect_literal("false"); h_.bool_value(false); return;
            case 'n': expect_literal("null"); h_.null_value(); return;
            case 'N':
                if (!o_.allow_nan_inf) fail("unexpected character 'N'");
                expect_literal("NaN");
                h_.real_value(std::numeric_limits<double>::quiet_NaN());
                return;
            case 'I':
                if (!o_.allow_nan_inf) fail("unexpected character 'I'");
                expect_literal("Infinity");
                h_.real_value(std::numeric_limits<double>::infinity());
                return;
            default: break;
        }
        const char c = peek();
        if (c == '-' || (c >= '0' && c <= '9')) {
            number();
            return;
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    void object(usize depth) {
        if (depth >= o_.max_depth) fail("maximum nesting depth exceeded");
        ++pos_;  // {
        h_.start_object();
        skip_ws();
        if (peek() == '}') { ++pos_; h_.end_object(); return; }
        for (;;) {
            skip_ws();
            if (peek() != '"') fail("expected a string key");
            std::string k;
            string_into(k);
            h_.key(k);
            skip_ws();
            if (peek() != ':') fail("expected ':'");
            ++pos_;
            skip_ws();
            value(depth + 1);
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                if (o_.allow_trailing_commas) {
                    skip_ws();
                    if (peek() == '}') { ++pos_; break; }
                }
                continue;
            }
            if (peek() == '}') { ++pos_; break; }
            fail("expected ',' or '}'");
        }
        h_.end_object();
    }

    void array(usize depth) {
        if (depth >= o_.max_depth) fail("maximum nesting depth exceeded");
        ++pos_;  // [
        h_.start_array();
        skip_ws();
        if (peek() == ']') { ++pos_; h_.end_array(); return; }
        for (;;) {
            skip_ws();
            value(depth + 1);
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                if (o_.allow_trailing_commas) {
                    skip_ws();
                    if (peek() == ']') { ++pos_; break; }
                }
                continue;
            }
            if (peek() == ']') { ++pos_; break; }
            fail("expected ',' or ']'");
        }
        h_.end_array();
    }

    [[nodiscard]] static int hex_val(char c) noexcept {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    char32_t hex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        char32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_val(s_[pos_ + static_cast<usize>(i)]);
            if (h < 0) { pos_ += static_cast<usize>(i); fail("invalid \\u escape"); }
            v = (v << 4) | static_cast<char32_t>(h);
        }
        pos_ += 4;
        return v;
    }

    void string_into(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            // Copy the longest run of ordinary characters in one go.
            const usize start = pos_;
            while (pos_ < s_.size()) {
                const auto c = static_cast<unsigned char>(s_[pos_]);
                if (c == '"' || c == '\\' || c < 0x20) break;
                ++pos_;
            }
            if (pos_ > start) {
                const std::string_view run = s_.substr(start, pos_ - start);
                if (!utf8::is_valid(run)) {
                    usize p = 0;
                    char32_t cp = 0;
                    while (p < run.size() && utf8::decode(run, p, cp)) {}
                    pos_ = start + p - 1;
                    fail("invalid UTF-8 in string");
                }
                out.append(run);
            }
            if (at_end()) fail("unterminated string");
            const char c = s_[pos_];
            if (c == '"') { ++pos_; return; }
            if (static_cast<unsigned char>(c) < 0x20) fail("unescaped control character in string");
            ++pos_;  // backslash
            if (at_end()) fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    char32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            const char32_t lo = hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            fail("unpaired high surrogate");
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("unpaired low surrogate");
                    }
                    utf8::append(out, cp);
                    break;
                }
                default: --pos_; fail(std::string("invalid escape '\\") + e + "'");
            }
        }
    }

    void number() {
        const usize start = pos_;
        bool is_real = false;
        if (peek() == '-') {
            ++pos_;
            if (o_.allow_nan_inf && peek() == 'I') {
                expect_literal("Infinity");
                h_.real_value(-std::numeric_limits<double>::infinity());
                return;
            }
        }
        if (peek() == '0') {
            ++pos_;
            if (peek() >= '0' && peek() <= '9') fail("leading zeros are not allowed");
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            fail("invalid number");
        }
        if (peek() == '.') {
            is_real = true;
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("digit expected after '.'");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            is_real = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("digit expected in exponent");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        const char* b = s_.data() + start;
        const char* e = s_.data() + pos_;
        if (!is_real) {
            i64 v = 0;
            const auto r = std::from_chars(b, e, v);
            if (r.ec == std::errc() && r.ptr == e) {
                h_.integer_value(v);
                return;
            }
            // too large for i64: fall through to double
        }
        double d = 0;
        const auto r = std::from_chars(b, e, d);
        if (r.ptr != e || r.ec == std::errc::invalid_argument) fail("invalid number");
        if (r.ec == std::errc::result_out_of_range) {
            // Overflow -> +-infinity, underflow (negative exponent) -> +-0.
            const std::string_view txt(b, static_cast<usize>(e - b));
            const auto ep = txt.find_first_of("eE");
            const bool underflow = ep != std::string_view::npos && ep + 1 < txt.size() && txt[ep + 1] == '-';
            const double mag = underflow ? 0.0 : std::numeric_limits<double>::infinity();
            d = *b == '-' ? -mag : mag;
        }
        h_.real_value(d);
    }

    std::string_view s_;
    usize pos_ = 0;
    Handler& h_;
    const ParseOptions& o_;
};

/// Builds a Value tree from events.
class Builder final : public Handler {
public:
    explicit Builder(bool reject_dups) : reject_dups_(reject_dups) {}
    Value take() { return std::move(root_); }

    void null_value() override { put(Value()); }
    void bool_value(bool b) override { put(Value(b)); }
    void integer_value(i64 i) override { put(Value(i)); }
    void real_value(double d) override { put(Value(d)); }
    void string_value(std::string_view s) override { put(Value(s)); }
    void start_object() override { stack_.push_back(Frame{Value(Object{}), {}}); }
    void key(std::string_view k) override {
        auto& f = stack_.back();
        if (reject_dups_ && f.v.as_object().contains(k))
            throw DataException("JSON: duplicate object key '" + std::string(k) + "'");
        f.key.assign(k);
    }
    void end_object() override { close(); }
    void start_array() override { stack_.push_back(Frame{Value(Array{}), {}}); }
    void end_array() override { close(); }

private:
    struct Frame {
        Value v;
        std::string key;
    };
    void close() {
        Value done = std::move(stack_.back().v);
        stack_.pop_back();
        put(std::move(done));
    }
    void put(Value v) {
        if (stack_.empty()) {
            root_ = std::move(v);
            return;
        }
        Frame& f = stack_.back();
        if (f.v.is_array()) f.v.as_array().push_back(std::move(v));
        else f.v.as_object().insert_or_assign(std::move(f.key), std::move(v));
    }
    std::vector<Frame> stack_;
    Value root_;
    bool reject_dups_;
};

}  // namespace detail

/// Parses `text` and feeds `handler`. Throws DataException (with line/column) on malformed input.
inline void parse_events(std::string_view text, Handler& handler, const ParseOptions& opt = {}) {
    detail::Parser(text, handler, opt).parse_document();
}

[[nodiscard]] inline Value parse(std::string_view text, const ParseOptions& opt = {}) {
    detail::Builder b(opt.reject_duplicate_keys);
    parse_events(text, b, opt);
    return b.take();
}

/// Like parse(), but returns false instead of throwing.
[[nodiscard]] inline bool try_parse(std::string_view text, Value& out, const ParseOptions& opt = {}) noexcept {
    try {
        out = parse(text, opt);
        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------
namespace detail {

inline void write_string(std::string& out, std::string_view s, bool ascii_only) {
    if (!utf8::is_valid(s)) throw DataException("JSON: cannot write a string that is not valid UTF-8");
    out += '"';
    usize pos = 0;
    while (pos < s.size()) {
        const auto c = static_cast<unsigned char>(s[pos]);
        if (c < 0x80) {
            ++pos;
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        static constexpr char hex[] = "0123456789abcdef";
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 15];
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        } else if (!ascii_only) {
            const usize start = pos;
            char32_t cp = 0;
            (void)utf8::decode(s, pos, cp);
            out.append(s.substr(start, pos - start));
        } else {
            char32_t cp = 0;
            (void)utf8::decode(s, pos, cp);
            auto esc = [&](unsigned v) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u";
                out += hex[(v >> 12) & 15];
                out += hex[(v >> 8) & 15];
                out += hex[(v >> 4) & 15];
                out += hex[v & 15];
            };
            if (cp >= 0x10000) {
                cp -= 0x10000;
                esc(0xD800 + static_cast<unsigned>(cp >> 10));
                esc(0xDC00 + static_cast<unsigned>(cp & 0x3FF));
            } else {
                esc(static_cast<unsigned>(cp));
            }
        }
    }
    out += '"';
}

inline void write_double(std::string& out, double d, bool allow_nan_inf) {
    if (std::isnan(d)) {
        if (!allow_nan_inf) throw DataException("JSON: NaN cannot be represented");
        out += "NaN";
        return;
    }
    if (std::isinf(d)) {
        if (!allow_nan_inf) throw DataException("JSON: infinity cannot be represented");
        out += d < 0 ? "-Infinity" : "Infinity";
        return;
    }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, d);  // shortest round-trip form
    std::string_view sv(buf, static_cast<usize>(r.ptr - buf));
    out += sv;
    if (sv.find_first_of(".eE") == std::string_view::npos) out += ".0";  // keep it a real on re-parse
}

inline void write_value(std::string& out, const Value& v, const WriteOptions& o, int level) {
    const bool pretty = o.indent >= 0;
    auto newline = [&](int lvl) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<usize>(lvl) * static_cast<usize>(o.indent), ' ');
    };
    switch (v.type()) {
        case Type::null: out += "null"; break;
        case Type::boolean: out += v.as_bool() ? "true" : "false"; break;
        case Type::integer: out += std::to_string(v.as_int()); break;
        case Type::real: write_double(out, v.as_double(), o.allow_nan_inf); break;
        case Type::string: write_string(out, v.as_string(), o.ascii_only); break;
        case Type::binary: {
            if (!o.binary_as_base64) throw DataException("JSON: binary values are not supported");
            write_string(out, base64_encode(v.as_binary()), o.ascii_only);
            break;
        }
        case Type::array: {
            const auto& a = v.as_array();
            if (a.empty()) { out += "[]"; break; }
            out += '[';
            for (usize i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                newline(level + 1);
                write_value(out, a[i], o, level + 1);
            }
            newline(level);
            out += ']';
            break;
        }
        case Type::object: {
            const auto& obj = v.as_object();
            if (obj.empty()) { out += "{}"; break; }
            std::vector<const Object::Entry*> order;
            order.reserve(obj.size());
            for (const auto& e : obj) order.push_back(&e);
            if (o.sort_keys)
                std::sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->first < b->first; });
            out += '{';
            bool first = true;
            for (const auto* e : order) {
                if (!first) out += ',';
                first = false;
                newline(level + 1);
                write_string(out, e->first, o.ascii_only);
                out += pretty ? ": " : ":";
                write_value(out, e->second, o, level + 1);
            }
            newline(level);
            out += '}';
            break;
        }
    }
}
}  // namespace detail

[[nodiscard]] inline std::string stringify(const Value& v, const WriteOptions& opt = {}) {
    std::string out;
    detail::write_value(out, v, opt, 0);
    return out;
}

[[nodiscard]] inline std::string pretty(const Value& v, int indent = 2) {
    WriteOptions o;
    o.indent = indent;
    return stringify(v, o);
}

/// Incremental writer producing a syntactically valid document and checking call order.
/// Output is appended to an internal buffer that can be drained with take() or flushed to a stream.
class Writer {
public:
    explicit Writer(WriteOptions opt = {}) : opt_(opt) {}
    Writer(std::ostream& sink, WriteOptions opt = {}) : opt_(opt), sink_(&sink) {}

    Writer& begin_object() { before_value(); out_ += '{'; stack_.push_back({true, true, false}); return *this; }
    Writer& end_object() {
        if (stack_.empty() || !stack_.back().object) throw DataException("JSON writer: end_object without begin_object");
        if (stack_.back().expect_value) throw DataException("JSON writer: key without a value");
        const bool had = !stack_.back().first;
        stack_.pop_back();
        if (had) newline();
        out_ += '}';
        after_value();
        return *this;
    }
    Writer& begin_array() { before_value(); out_ += '['; stack_.push_back({false, true, false}); return *this; }
    Writer& end_array() {
        if (stack_.empty() || stack_.back().object) throw DataException("JSON writer: end_array without begin_array");
        const bool had = !stack_.back().first;
        stack_.pop_back();
        if (had) newline();
        out_ += ']';
        after_value();
        return *this;
    }
    Writer& key(std::string_view k) {
        if (stack_.empty() || !stack_.back().object || stack_.back().expect_value)
            throw DataException("JSON writer: key outside of an object or after another key");
        if (!stack_.back().first) out_ += ',';
        stack_.back().first = false;
        newline();
        detail::write_string(out_, k, opt_.ascii_only);
        out_ += opt_.indent >= 0 ? ": " : ":";
        stack_.back().expect_value = true;
        return *this;
    }
    Writer& value(const Value& v) {
        before_value();
        std::string tmp;
        detail::write_value(tmp, v, opt_, static_cast<int>(stack_.size()));
        out_ += tmp;
        after_value();
        return *this;
    }
    Writer& null() { return value(Value()); }
    template <class T>
        requires(!std::same_as<std::remove_cvref_t<T>, Value>)
    Writer& value(T&& v) { return value(Value(std::forward<T>(v))); }

    /// Moves out what has been produced so far (and writes it to the sink, if one was given).
    std::string take() {
        std::string s = std::move(out_);
        out_.clear();
        return s;
    }
    void flush() {
        if (sink_) {
            sink_->write(out_.data(), static_cast<std::streamsize>(out_.size()));
            out_.clear();
        }
    }
    /// True when exactly one complete top-level value has been written.
    [[nodiscard]] bool complete() const noexcept { return stack_.empty() && wrote_root_; }
    [[nodiscard]] const std::string& buffer() const noexcept { return out_; }

private:
    struct Frame {
        bool object;
        bool first;
        bool expect_value;
    };
    void newline() {
        if (opt_.indent < 0) return;
        out_ += '\n';
        out_.append(stack_.size() * static_cast<usize>(opt_.indent), ' ');
    }
    void before_value() {
        if (stack_.empty()) {
            if (wrote_root_) throw DataException("JSON writer: more than one top-level value");
            return;
        }
        Frame& f = stack_.back();
        if (f.object) {
            if (!f.expect_value) throw DataException("JSON writer: object member needs a key first");
        } else {
            if (!f.first) out_ += ',';
            f.first = false;
            newline();
        }
    }
    void after_value() {
        if (stack_.empty()) wrote_root_ = true;
        else stack_.back().expect_value = false;
        if (sink_ && out_.size() > 64 * 1024) flush();
    }
    WriteOptions opt_;
    std::ostream* sink_ = nullptr;
    std::string out_;
    std::vector<Frame> stack_;
    bool wrote_root_ = false;

};

/// Streams newline-delimited JSON (one document per line). Blank lines are skipped. The callback
/// returns false to stop early. Returns the number of documents delivered.
inline usize read_ndjson(std::istream& in, const std::function<bool(Value&&)>& on_value, const ParseOptions& opt = {}) {
    std::string line;
    usize n = 0, line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        Value v;
        try {
            v = parse(line, opt);
        } catch (const DataException& e) {
            throw DataException("NDJSON line " + std::to_string(line_no) + ": " + e.message(), e.offset());
        }
        ++n;
        if (!on_value(std::move(v))) break;
    }
    return n;
}

inline void write_ndjson_line(std::ostream& out, const Value& v, WriteOptions opt = {}) {
    opt.indent = -1;
    out << stringify(v, opt) << '\n';
}

// ---------------------------------------------------------------------------------------------
// JSON Pointer (RFC 6901) and Merge Patch (RFC 7386)
// ---------------------------------------------------------------------------------------------

/// Resolves "/a/0/b" ("~1" = '/', "~0" = '~'). The empty pointer selects the whole document.
[[nodiscard]] inline const Value* pointer(const Value& root, std::string_view ptr) {
    if (ptr.empty()) return &root;
    if (ptr[0] != '/') throw DataException("JSON pointer must start with '/'");
    const Value* cur = &root;
    usize i = 1;
    while (i <= ptr.size()) {
        const auto j = ptr.find('/', i);
        const auto e = j == std::string_view::npos ? ptr.size() : j;
        std::string tok;
        for (usize k = i; k < e; ++k) {
            if (ptr[k] == '~') {
                if (k + 1 >= e || (ptr[k + 1] != '0' && ptr[k + 1] != '1')) throw DataException("bad '~' escape in JSON pointer", k);
                tok += ptr[++k] == '0' ? '~' : '/';
            } else {
                tok += ptr[k];
            }
        }
        if (cur->is_object()) {
            cur = cur->find(tok);
        } else if (cur->is_array()) {
            if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos || (tok.size() > 1 && tok[0] == '0')) return nullptr;
            const usize idx = std::stoull(tok);
            cur = idx < cur->as_array().size() ? &cur->as_array()[idx] : nullptr;
        } else {
            return nullptr;
        }
        if (!cur) return nullptr;
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return cur;
}

/// RFC 7386: objects merge recursively, null deletes a member, anything else replaces.
inline void merge_patch(Value& target, const Value& patch) {
    if (!patch.is_object()) {
        target = patch;
        return;
    }
    if (!target.is_object()) target = Value(Object{});
    for (const auto& [k, v] : patch.as_object()) {
        if (v.is_null()) {
            target.as_object().erase(k);
        } else {
            Value& slot = target.as_object()[k];
            merge_patch(slot, v);
        }
    }
}

}  // namespace flyg::data::json
