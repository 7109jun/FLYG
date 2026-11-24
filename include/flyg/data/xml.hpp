// flyG - XML 1.0 (non-validating, namespace-agnostic): DOM parser, writer, Value bridge
// SPDX-License-Identifier: GPL-2.0-only
//
// Security: DOCTYPE declarations are skipped; no entity is ever defined or expanded beyond the
// five predefined ones and numeric character references, so billion-laughs / XXE are impossible.
#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data::xml {

struct Node {
    enum class Kind { element, text, cdata, comment, processing_instruction };

    Kind kind = Kind::element;
    std::string name;   ///< element name, or the target of a processing instruction
    std::string text;   ///< character data / comment text / PI data
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<Node> children;

    [[nodiscard]] static Node element(std::string name) {
        Node n;
        n.name = std::move(name);
        return n;
    }
    [[nodiscard]] static Node make_text(std::string t) {
        Node n;
        n.kind = Kind::text;
        n.text = std::move(t);
        return n;
    }

    [[nodiscard]] bool is_element() const noexcept { return kind == Kind::element; }

    [[nodiscard]] const std::string* attribute(std::string_view n) const {
        for (const auto& a : attributes)
            if (a.first == n) return &a.second;
        return nullptr;
    }
    [[nodiscard]] std::string attribute_or(std::string_view n, std::string fallback) const {
        const auto* a = attribute(n);
        return a ? *a : std::move(fallback);
    }
    void set_attribute(std::string n, std::string v) {
        for (auto& a : attributes)
            if (a.first == n) {
                a.second = std::move(v);
                return;
            }
        attributes.emplace_back(std::move(n), std::move(v));
    }
    Node& add_child(Node c) {
        children.push_back(std::move(c));
        return children.back();
    }
    [[nodiscard]] const Node* first_child(std::string_view n) const {
        for (const auto& c : children)
            if (c.is_element() && c.name == n) return &c;
        return nullptr;
    }
    [[nodiscard]] std::vector<const Node*> children_named(std::string_view n) const {
        std::vector<const Node*> v;
        for (const auto& c : children)
            if (c.is_element() && c.name == n) v.push_back(&c);
        return v;
    }
    /// Concatenated text and CDATA of this node and all descendants.
    [[nodiscard]] std::string inner_text() const {
        if (kind == Kind::text || kind == Kind::cdata) return text;
        std::string s;
        for (const auto& c : children)
            if (c.kind != Kind::comment && c.kind != Kind::processing_instruction) s += c.inner_text();
        return s;
    }
    /// Namespace-prefix-free name ("a:b" -> "b").
    [[nodiscard]] std::string_view local_name() const noexcept {
        const auto p = name.find(':');
        return p == std::string::npos ? std::string_view(name) : std::string_view(name).substr(p + 1);
    }
};

struct Document {
    std::string version = "1.0";
    std::string encoding = "UTF-8";
    std::optional<bool> standalone;
    std::vector<Node> prolog;  ///< comments / PIs before the root
    Node root;
    std::vector<Node> epilog;  ///< comments / PIs after the root
};

struct ParseOptions {
    bool keep_whitespace = false;  ///< keep whitespace-only text nodes
    bool keep_comments = true;
    usize max_depth = 256;
};

namespace detail {

[[nodiscard]] inline bool is_name_start(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
}
[[nodiscard]] inline bool is_name_char(unsigned char c) noexcept {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}
[[nodiscard]] inline bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

class Parser {
public:
    Parser(std::string_view s, const ParseOptions& o) : s_(s), o_(o) {}

    Document document() {
        Document d;
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
        if (starts("<?xml") && pos_ + 5 < s_.size() && is_ws(s_[pos_ + 5])) xml_decl(d);
        bool have_root = false;
        for (;;) {
            skip_ws();
            if (pos_ >= s_.size()) break;
            if (starts("<!--")) {
                Node c = comment();
                (have_root ? d.epilog : d.prolog).push_back(std::move(c));
            } else if (starts("<?")) {
                Node p = pi();
                (have_root ? d.epilog : d.prolog).push_back(std::move(p));
            } else if (starts("<!DOCTYPE")) {
                if (have_root) fail("DOCTYPE after the root element");
                doctype();
            } else if (starts("<")) {
                if (have_root) fail("more than one root element");
                d.root = element(0);
                have_root = true;
            } else {
                fail("text outside of the root element");
            }
        }
        if (!have_root) fail("no root element");
        return d;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        usize line = 1, col = 1;
        for (usize i = 0; i < pos_ && i < s_.size(); ++i) {
            if (s_[i] == '\n') { ++line; col = 1; }
            else ++col;
        }
        throw DataException("XML: " + what + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")", pos_);
    }
    [[nodiscard]] bool starts(std::string_view p) const noexcept { return s_.compare(pos_, p.size(), p) == 0; }
    void skip_ws() {
        while (pos_ < s_.size() && is_ws(s_[pos_])) ++pos_;
    }

    std::string name() {
        if (pos_ >= s_.size() || !is_name_start(static_cast<unsigned char>(s_[pos_]))) fail("name expected");
        const usize b = pos_++;
        while (pos_ < s_.size() && is_name_char(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        return std::string(s_.substr(b, pos_ - b));
    }

    void xml_decl(Document& d) {
        pos_ += 5;
        for (;;) {
            skip_ws();
            if (starts("?>")) { pos_ += 2; return; }
            const std::string k = name();
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '=') fail("'=' expected in XML declaration");
            ++pos_;
            skip_ws();
            const std::string v = quoted_raw();
            if (k == "version") d.version = v;
            else if (k == "encoding") {
                d.encoding = v;
                std::string up;
                for (char c : v) up += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
                if (up != "UTF-8" && up != "UTF8" && up != "US-ASCII" && up != "ASCII")
                    fail("unsupported encoding '" + v + "' (only UTF-8 is supported)");
            } else if (k == "standalone") d.standalone = (v == "yes");
            else fail("unknown XML declaration attribute '" + k + "'");
        }
    }

    std::string quoted_raw() {
        if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) fail("quoted value expected");
        const char q = s_[pos_++];
        const auto e = s_.find(q, pos_);
        if (e == std::string_view::npos) fail("unterminated quoted value");
        std::string v(s_.substr(pos_, e - pos_));
        pos_ = e + 1;
        return v;
    }

    void doctype() {
        pos_ += 9;
        int bracket = 0;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '"' || c == '\'') {
                const auto e = s_.find(c, pos_ + 1);
                if (e == std::string_view::npos) fail("unterminated string in DOCTYPE");
                pos_ = e + 1;
                continue;
            }
            if (starts("<!--")) {
                const auto e = s_.find("-->", pos_ + 4);
                if (e == std::string_view::npos) fail("unterminated comment in DOCTYPE");
                pos_ = e + 3;
                continue;
            }
            if (c == '[') ++bracket;
            else if (c == ']') --bracket;
            else if (c == '>' && bracket <= 0) { ++pos_; return; }
            ++pos_;
        }
        fail("unterminated DOCTYPE");
    }

    Node comment() {
        pos_ += 4;
        const auto e = s_.find("--", pos_);
        if (e == std::string_view::npos) fail("unterminated comment");
        if (e + 2 >= s_.size() || s_[e + 2] != '>') fail("'--' is not allowed inside a comment");
        Node n;
        n.kind = Node::Kind::comment;
        n.text = std::string(s_.substr(pos_, e - pos_));
        pos_ = e + 3;
        return n;
    }

    Node pi() {
        pos_ += 2;
        Node n;
        n.kind = Node::Kind::processing_instruction;
        n.name = name();
        if (n.name.size() == 3 && (n.name[0] | 32) == 'x' && (n.name[1] | 32) == 'm' && (n.name[2] | 32) == 'l')
            fail("reserved processing instruction target 'xml'");
        const auto e = s_.find("?>", pos_);
        if (e == std::string_view::npos) fail("unterminated processing instruction");
        usize b = pos_;
        while (b < e && is_ws(s_[b])) ++b;
        n.text = std::string(s_.substr(b, e - b));
        pos_ = e + 2;
        return n;
    }

    void append_reference(std::string& out) {
        // at '&'
        const auto semi = s_.find(';', pos_);
        if (semi == std::string_view::npos || semi - pos_ > 12) fail("malformed entity reference");
        const std::string_view ref = s_.substr(pos_ + 1, semi - pos_ - 1);
        if (ref == "lt") out += '<';
        else if (ref == "gt") out += '>';
        else if (ref == "amp") out += '&';
        else if (ref == "quot") out += '"';
        else if (ref == "apos") out += '\'';
        else if (!ref.empty() && ref[0] == '#') {
            char32_t cp = 0;
            const bool hex = ref.size() > 1 && ref[1] == 'x';
            const usize start = hex ? 2 : 1;
            if (start >= ref.size()) fail("empty character reference");
            for (usize i = start; i < ref.size(); ++i) {
                const char c = ref[i];
                int d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else fail("invalid character reference");
                cp = cp * (hex ? 16 : 10) + static_cast<char32_t>(d);
                if (cp > 0x10FFFF) fail("character reference out of range");
            }
            const bool ok = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
                            (cp >= 0x10000 && cp <= 0x10FFFF);
            if (!ok) fail("character reference to a forbidden code point");
            utf8::append(out, cp);
        } else {
            fail("undefined entity '&" + std::string(ref) + ";'");
        }
        pos_ = semi + 1;
    }

    std::string attribute_value() {
        if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) fail("quoted attribute value expected");
        const char q = s_[pos_++];
        std::string out;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated attribute value");
            const char c = s_[pos_];
            if (c == q) { ++pos_; return out; }
            if (c == '<') fail("'<' is not allowed in an attribute value");
            if (c == '&') { append_reference(out); continue; }
            // attribute value normalization: literal whitespace characters become spaces
            if (c == '\n' || c == '\t' || c == '\r') {
                if (c == '\r' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '\n') ++pos_;
                out += ' ';
            } else {
                out += c;
            }
            ++pos_;
        }
    }

    Node element(usize depth) {
        if (depth >= o_.max_depth) fail("maximum nesting depth exceeded");
        ++pos_;  // '<'
        Node n;
        n.name = name();
        for (;;) {
            const usize before = pos_;
            skip_ws();
            if (pos_ >= s_.size()) fail("unterminated start tag");
            if (s_[pos_] == '>') { ++pos_; break; }
            if (starts("/>")) { pos_ += 2; return n; }
            if (pos_ == before) fail("whitespace expected between attributes");
            std::string an = name();
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '=') fail("'=' expected after attribute name");
            ++pos_;
            skip_ws();
            if (n.attribute(an)) fail("duplicate attribute '" + an + "'");
            n.attributes.emplace_back(std::move(an), attribute_value());
        }
        content(n, depth);
        return n;
    }

    void flush_text(Node& parent, std::string& text, bool& had_cdata_or_ref) {
        if (text.empty()) return;
        const bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
        if (!blank || o_.keep_whitespace || had_cdata_or_ref) parent.children.push_back(Node::make_text(std::move(text)));
        text.clear();
        had_cdata_or_ref = false;
    }

    void content(Node& parent, usize depth) {
        std::string text;
        bool special = false;
        for (;;) {
            if (pos_ >= s_.size()) fail("unterminated element <" + parent.name + ">");
            const char c = s_[pos_];
            if (c == '<') {
                if (starts("</")) {
                    flush_text(parent, text, special);
                    pos_ += 2;
                    const std::string close = name();
                    if (close != parent.name) fail("mismatched closing tag </" + close + ">, expected </" + parent.name + ">");
                    skip_ws();
                    if (pos_ >= s_.size() || s_[pos_] != '>') fail("'>' expected");
                    ++pos_;
                    return;
                }
                if (starts("<!--")) {
                    Node cm = comment();  // a dropped comment must not split the surrounding text
                    if (o_.keep_comments) {
                        flush_text(parent, text, special);
                        parent.children.push_back(std::move(cm));
                    }
                } else if (starts("<![CDATA[")) {
                    const auto e = s_.find("]]>", pos_ + 9);
                    if (e == std::string_view::npos) fail("unterminated CDATA section");
                    flush_text(parent, text, special);
                    Node cd;
                    cd.kind = Node::Kind::cdata;
                    cd.text = std::string(s_.substr(pos_ + 9, e - pos_ - 9));
                    pos_ = e + 3;
                    parent.children.push_back(std::move(cd));
                } else if (starts("<?")) {
                    flush_text(parent, text, special);
                    parent.children.push_back(pi());
                } else {
                    flush_text(parent, text, special);
                    parent.children.push_back(element(depth + 1));
                }
            } else if (c == '&') {
                special = true;
                append_reference(text);
            } else if (c == '\r') {  // normalize line ends to \n
                text += '\n';
                ++pos_;
                if (pos_ < s_.size() && s_[pos_] == '\n') ++pos_;
            } else {
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') fail("control character in text");
                if (c == ']' && starts("]]>")) fail("']]>' is not allowed in text");
                text += c;
                ++pos_;
            }
        }
    }

    std::string_view s_;
    const ParseOptions& o_;
    usize pos_ = 0;
};

inline void escape_text(std::string& out, std::string_view t) {
    for (char c : t) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\r': out += "&#13;"; break;
            default: out += c;
        }
    }
}
inline void escape_attr(std::string& out, std::string_view t) {
    for (char c : t) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "&#10;"; break;
            case '\r': out += "&#13;"; break;
            case '\t': out += "&#9;"; break;
            default: out += c;
        }
    }
}

[[nodiscard]] inline bool valid_name(std::string_view n) noexcept {
    if (n.empty() || !is_name_start(static_cast<unsigned char>(n[0]))) return false;
    for (char c : n)
        if (!is_name_char(static_cast<unsigned char>(c))) return false;
    return true;
}

inline void write_node(std::string& out, const Node& n, int indent, int level) {
    auto pad = [&](int l) {
        if (indent >= 0) out.append(static_cast<usize>(l) * static_cast<usize>(indent), ' ');
    };
    const char* nl = indent >= 0 ? "\n" : "";
    switch (n.kind) {
        case Node::Kind::text: escape_text(out, n.text); return;
        case Node::Kind::cdata:
            if (n.text.find("]]>") != std::string::npos) throw DataException("XML: CDATA text contains ']]>'");
            out += "<![CDATA[" + n.text + "]]>";
            return;
        case Node::Kind::comment:
            if (n.text.find("--") != std::string::npos || (!n.text.empty() && n.text.back() == '-'))
                throw DataException("XML: comment text contains '--'");
            out += "<!--" + n.text + "-->";
            return;
        case Node::Kind::processing_instruction:
            if (!valid_name(n.name) || n.text.find("?>") != std::string::npos) throw DataException("XML: invalid processing instruction");
            out += "<?" + n.name + (n.text.empty() ? "" : " " + n.text) + "?>";
            return;
        case Node::Kind::element: break;
    }
    if (!valid_name(n.name)) throw DataException("XML: invalid element name '" + n.name + "'");
    out += '<';
    out += n.name;
    for (const auto& [k, v] : n.attributes) {
        if (!valid_name(k)) throw DataException("XML: invalid attribute name '" + k + "'");
        out += ' ';
        out += k;
        out += "=\"";
        escape_attr(out, v);
        out += '"';
    }
    if (n.children.empty()) {
        out += "/>";
        return;
    }
    out += '>';
    // Pretty-print only when no child is character data (mixed content must stay untouched).
    bool pretty_children = indent >= 0;
    for (const auto& c : n.children)
        if (c.kind == Node::Kind::text || c.kind == Node::Kind::cdata) pretty_children = false;
    for (const auto& c : n.children) {
        if (pretty_children) {
            out += nl;
            pad(level + 1);
        }
        write_node(out, c, pretty_children ? indent : -1, level + 1);
    }
    if (pretty_children) {
        out += nl;
        pad(level);
    }
    out += "</" + n.name + ">";
}
}  // namespace detail

[[nodiscard]] inline Document parse(std::string_view text, const ParseOptions& opt = {}) {
    if (!utf8::is_valid(text)) throw DataException("XML: input is not valid UTF-8");
    return detail::Parser(text, opt).document();
}

struct WriteOptions {
    int indent = -1;               ///< < 0 compact
    bool xml_declaration = true;
};

[[nodiscard]] inline std::string write(const Node& root, const WriteOptions& o = {}) {
    std::string out;
    if (o.xml_declaration) out += std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") + (o.indent >= 0 ? "\n" : "");
    detail::write_node(out, root, o.indent, 0);
    if (o.indent >= 0) out += '\n';
    return out;
}

[[nodiscard]] inline std::string write(const Document& d, const WriteOptions& o = {}) {
    std::string out;
    if (o.xml_declaration) {
        out += "<?xml version=\"" + d.version + "\" encoding=\"UTF-8\"";
        if (d.standalone) out += *d.standalone ? " standalone=\"yes\"" : " standalone=\"no\"";
        out += "?>";
        if (o.indent >= 0) out += '\n';
    }
    for (const auto& n : d.prolog) {
        detail::write_node(out, n, -1, 0);
        if (o.indent >= 0) out += '\n';
    }
    detail::write_node(out, d.root, o.indent, 0);
    if (o.indent >= 0) out += '\n';
    for (const auto& n : d.epilog) {
        detail::write_node(out, n, -1, 0);
        if (o.indent >= 0) out += '\n';
    }
    return out;
}

/// Conventional XML -> Value mapping (the same one used by many JSON/XML converters):
///   <a x="1">text</a>          -> {"@x": "1", "#text": "text"}   (or just "text" without attributes)
///   repeated child elements    -> arrays
///   element without content    -> null (or {"@x": ...} with attributes)
/// Comments and processing instructions are dropped; text is trimmed when an element also has
/// child elements.
[[nodiscard]] inline Value to_value(const Node& n) {
    Object o;
    for (const auto& [k, v] : n.attributes) o.append_unchecked("@" + k, Value(v));
    std::string text;
    bool has_elements = false;
    for (const auto& c : n.children) {
        if (c.is_element()) {
            has_elements = true;
            Value cv = to_value(c);
            if (Value* existing = o.find(c.name)) {
                if (!existing->is_array()) {  // first duplicate: promote to array
                    Value arr = Value(Array{});
                    arr.as_array().push_back(std::move(*existing));
                    *existing = std::move(arr);
                }
                existing->as_array().push_back(std::move(cv));
            } else {
                o.append_unchecked(c.name, std::move(cv));
            }
        } else if (c.kind == Node::Kind::text || c.kind == Node::Kind::cdata) {
            text += c.text;
        }
    }
    if (has_elements) {
        const auto b = text.find_first_not_of(" \t\r\n");
        text = b == std::string::npos ? std::string{} : text.substr(b, text.find_last_not_of(" \t\r\n") - b + 1);
    }
    if (!text.empty()) {
        if (o.empty()) return Value(std::move(text));
        o.append_unchecked("#text", Value(std::move(text)));
    }
    if (o.empty()) return Value();
    return Value(std::move(o));
}

/// Inverse of to_value for an element named `name`.
[[nodiscard]] inline Node from_value(std::string name, const Value& v) {
    Node n = Node::element(std::move(name));
    switch (v.type()) {
        case Type::null: break;
        case Type::object:
            for (const auto& [k, x] : v.as_object()) {
                if (!k.empty() && k[0] == '@') {
                    n.set_attribute(k.substr(1), x.is_string() ? x.as_string() : x.is_null() ? std::string{} : x.is_bool() ? (x.as_bool() ? "true" : "false") : x.is_integer() ? std::to_string(x.as_int()) : x.is_real() ? std::to_string(x.as_double()) : throw DataException("XML: attribute '" + k + "' must be a scalar"));
                } else if (k == "#text") {
                    n.add_child(Node::make_text(x.is_string() ? x.as_string() : throw DataException("XML: #text must be a string")));
                } else if (x.is_array()) {
                    for (const auto& item : x.as_array()) n.add_child(from_value(k, item));
                } else {
                    n.add_child(from_value(k, x));
                }
            }
            break;
        case Type::array: throw DataException("XML: an array cannot be the content of a single element");
        case Type::boolean: n.add_child(Node::make_text(v.as_bool() ? "true" : "false")); break;
        case Type::integer: n.add_child(Node::make_text(std::to_string(v.as_int()))); break;
        case Type::real: {
            char buf[40];
            const auto r = std::to_chars(buf, buf + sizeof buf, v.as_double());
            n.add_child(Node::make_text(std::string(buf, r.ptr)));
            break;
        }
        case Type::string: n.add_child(Node::make_text(v.as_string())); break;
        case Type::binary: n.add_child(Node::make_text(base64_encode(v.as_binary()))); break;
    }
    return n;
}

}  // namespace flyg::data::xml
