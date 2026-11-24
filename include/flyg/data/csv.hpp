// flyG - CSV (RFC 4180 with common extensions): parser, writer, streaming reader
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <charconv>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/data/encoding.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data::csv {

using Row = std::vector<std::string>;

struct Options {
    char delimiter = ',';
    char quote = '"';
    bool skip_empty_lines = true;   ///< blank lines produce no row
    bool trim_unquoted = false;     ///< strip spaces/tabs around unquoted fields
    bool strict_quotes = true;      ///< throw on stray quotes / text after a closing quote
};

/// Pull parser over an in-memory buffer. Handles CRLF, LF and CR line ends, quoted fields with
/// embedded delimiters / newlines and doubled quotes.
class RowParser {
public:
    RowParser(std::string_view text, const Options& opt = {}) : s_(text), o_(opt) {
        if (s_.size() >= 3 && s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;  // BOM
        if (o_.delimiter == o_.quote || o_.delimiter == '\n' || o_.delimiter == '\r' || o_.quote == '\n' || o_.quote == '\r')
            throw DataException("CSV: invalid delimiter/quote combination");
    }

    /// Reads the next record; returns false at end of input.
    bool next(Row& row) {
        row.clear();
        for (;;) {
            if (pos_ >= s_.size()) return false;
            if (s_[pos_] == '\n' || s_[pos_] == '\r') {
                eat_newline();
                ++line_;
                if (o_.skip_empty_lines) continue;
                row.emplace_back();  // a blank line is a row with one empty field
                return true;
            }
            break;
        }
        for (;;) {
            row.push_back(field());
            if (pos_ >= s_.size()) break;
            const char c = s_[pos_];
            if (c == o_.delimiter) {
                ++pos_;
                if (pos_ >= s_.size()) {  // trailing delimiter at EOF: one more empty field
                    row.emplace_back();
                    break;
                }
                continue;
            }
            eat_newline();  // '\n' or '\r' ends the record
            ++line_;
            break;
        }
        ++records_;
        return true;
    }
    [[nodiscard]] usize line() const noexcept { return line_; }
    [[nodiscard]] usize records() const noexcept { return records_; }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw DataException("CSV: " + what + " (line " + std::to_string(line_) + ")", pos_);
    }
    void eat_newline() {
        if (s_[pos_] == '\r') {
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] == '\n') ++pos_;
        } else {
            ++pos_;
        }
    }
    std::string field() {
        std::string out;
        // optional leading blanks before an opening quote
        usize p = pos_;
        if (o_.trim_unquoted)
            while (p < s_.size() && (s_[p] == ' ' || s_[p] == '\t')) ++p;
        if (p < s_.size() && s_[p] == o_.quote) {
            pos_ = p + 1;
            for (;;) {
                if (pos_ >= s_.size()) fail("unterminated quoted field");
                const char c = s_[pos_];
                if (c == o_.quote) {
                    if (pos_ + 1 < s_.size() && s_[pos_ + 1] == o_.quote) {
                        out += o_.quote;
                        pos_ += 2;
                        continue;
                    }
                    ++pos_;
                    break;
                }
                if (c == '\n') ++line_;
                out += c;
                ++pos_;
            }
            // after the closing quote only blanks (if trimming) may precede the delimiter / EOL
            while (pos_ < s_.size() && o_.trim_unquoted && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
            if (pos_ < s_.size() && s_[pos_] != o_.delimiter && s_[pos_] != '\n' && s_[pos_] != '\r') {
                if (o_.strict_quotes) fail("unexpected character after closing quote");
                while (pos_ < s_.size() && s_[pos_] != o_.delimiter && s_[pos_] != '\n' && s_[pos_] != '\r') out += s_[pos_++];
            }
            return out;
        }
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == o_.delimiter || c == '\n' || c == '\r') break;
            if (c == o_.quote && o_.strict_quotes) fail("quote inside an unquoted field");
            out += c;
            ++pos_;
        }
        if (o_.trim_unquoted) {
            const auto b = out.find_first_not_of(" \t");
            if (b == std::string::npos) out.clear();
            else out = out.substr(b, out.find_last_not_of(" \t") - b + 1);
        }
        return out;
    }

    std::string_view s_;
    Options o_;
    usize pos_ = 0, line_ = 1, records_ = 0;
};

[[nodiscard]] inline std::vector<Row> parse_rows(std::string_view text, const Options& opt = {}) {
    std::vector<Row> rows;
    RowParser p(text, opt);
    Row r;
    while (p.next(r)) rows.push_back(r);
    return rows;
}

/// Streams records from an input stream (the whole stream is read in chunks; records are
/// delivered as soon as a complete one is available only for in-memory use, so very large files
/// should be processed with `read_stream`).
inline usize read_stream(std::istream& in, const std::function<bool(const Row&)>& on_row, const Options& opt = {}) {
    // Accumulate lines until the quote state is balanced, then parse that one record.
    std::string buf, line;
    usize n = 0;
    auto balanced = [&](const std::string& s) {
        bool inq = false;
        for (char c : s)
            if (c == opt.quote) inq = !inq;
        return !inq;
    };
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        first = false;
        buf += line;
        if (!balanced(buf)) {
            buf += '\n';
            continue;
        }
        if (!buf.empty() && buf.back() == '\r') buf.pop_back();
        RowParser p(buf, opt);
        Row r;
        while (p.next(r)) {
            ++n;
            if (!on_row(r)) return n;
        }
        buf.clear();
    }
    if (!buf.empty()) throw DataException("CSV: unterminated quoted field at end of stream");
    return n;
}

/// Converts text to a Value: with `header`, an array of objects keyed by the first row; without,
/// an array of arrays. With `infer_types`, unquoted-looking fields become null/bool/int/real.
struct TableOptions {
    Options csv;
    bool header = true;
    bool infer_types = false;
    bool strict_columns = true;  ///< throw when a row's field count differs from the header's
};

[[nodiscard]] inline Value infer_scalar(const std::string& f) {
    if (f.empty()) return Value();
    if (f == "true" || f == "TRUE" || f == "True") return Value(true);
    if (f == "false" || f == "FALSE" || f == "False") return Value(false);
    const char* b = f.data();
    const char* e = b + f.size();
    if (*b == '+' || (f.size() > 1 && *b == '0' && f[1] >= '0' && f[1] <= '9')) return Value(f);  // keep "+1", "007"
    i64 iv;
    if (auto r = std::from_chars(b, e, iv); r.ec == std::errc() && r.ptr == e) return Value(iv);
    double dv;
    if (auto r = std::from_chars(b, e, dv); r.ec == std::errc() && r.ptr == e && f.find_first_of("nNiI") == std::string::npos)
        return Value(dv);
    return Value(f);
}

[[nodiscard]] inline Value parse(std::string_view text, const TableOptions& opt = {}) {
    RowParser p(text, opt.csv);
    Row row;
    Array out;
    Row header;
    bool have_header = !opt.header;
    while (p.next(row)) {
        if (!have_header) {
            header = row;
            have_header = true;
            for (usize i = 0; i < header.size(); ++i)
                for (usize j = 0; j < i; ++j)
                    if (header[i] == header[j]) throw DataException("CSV: duplicate header column '" + header[i] + "'");
            continue;
        }
        auto conv = [&](const std::string& f) { return opt.infer_types ? infer_scalar(f) : Value(f); };
        if (opt.header) {
            if (opt.strict_columns && row.size() != header.size())
                throw DataException("CSV: row " + std::to_string(p.records()) + " has " + std::to_string(row.size()) + " fields, expected " + std::to_string(header.size()));
            Object o;
            for (usize i = 0; i < header.size(); ++i) o.append_unchecked(header[i], i < row.size() ? conv(row[i]) : Value());
            out.emplace_back(std::move(o));
        } else {
            Array a;
            for (const auto& f : row) a.push_back(conv(f));
            out.emplace_back(std::move(a));
        }
    }
    return Value(std::move(out));
}

// ---- writing -------------------------------------------------------------------------------------
[[nodiscard]] inline std::string quote_field(std::string_view f, const Options& o = {}, bool force = false) {
    const bool needs = force || f.empty() ? false : (f.find_first_of(std::string{o.delimiter, o.quote, '\n', '\r'}) != std::string_view::npos ||
                                                    f.front() == ' ' || f.back() == ' ' || f.front() == '\t' || f.back() == '\t');
    if (!needs && !force) return std::string(f);
    std::string out;
    out += o.quote;
    for (char c : f) {
        if (c == o.quote) out += o.quote;
        out += c;
    }
    out += o.quote;
    return out;
}

[[nodiscard]] inline std::string write_row(const Row& row, const Options& o = {}) {
    std::string out;
    for (usize i = 0; i < row.size(); ++i) {
        if (i) out += o.delimiter;
        // a lone empty field would otherwise be indistinguishable from a blank line
        out += quote_field(row[i], o, row.size() == 1 && row[i].empty());
    }
    return out;
}

[[nodiscard]] inline std::string write_rows(const std::vector<Row>& rows, const Options& o = {}, std::string_view eol = "\r\n") {
    std::string out;
    for (const auto& r : rows) {
        out += write_row(r, o);
        out += eol;
    }
    return out;
}

namespace detail {
[[nodiscard]] inline std::string cell_text(const Value& v) {
    switch (v.type()) {
        case Type::null: return {};
        case Type::boolean: return v.as_bool() ? "true" : "false";
        case Type::integer: return std::to_string(v.as_int());
        case Type::real: {
            char buf[40];
            const auto r = std::to_chars(buf, buf + sizeof buf, v.as_double());
            return std::string(buf, r.ptr);
        }
        case Type::string: return v.as_string();
        case Type::binary: return base64_encode(v.as_binary());
        default: throw DataException("CSV: nested arrays/objects cannot be written as a cell");
    }
}
}  // namespace detail

/// Writes an array of objects (header = union of keys in first-seen order) or an array of arrays.
[[nodiscard]] inline std::string write(const Value& table, const Options& o = {}, bool header = true, std::string_view eol = "\r\n") {
    const auto& rows = table.as_array();
    std::vector<Row> out;
    if (!rows.empty() && rows[0].is_object()) {
        std::vector<std::string> cols;
        for (const auto& r : rows) {
            if (!r.is_object()) throw DataException("CSV: mixed rows (objects and arrays)");
            for (const auto& [k, v] : r.as_object())
                if (std::find(cols.begin(), cols.end(), k) == cols.end()) cols.push_back(k);
        }
        if (header) out.push_back(cols);
        for (const auto& r : rows) {
            Row line;
            for (const auto& c : cols) {
                const Value* v = r.find(c);
                line.push_back(v ? detail::cell_text(*v) : std::string{});
            }
            out.push_back(std::move(line));
        }
    } else {
        for (const auto& r : rows) {
            if (!r.is_array()) throw DataException("CSV: rows must be arrays or objects");
            Row line;
            for (const auto& v : r.as_array()) line.push_back(detail::cell_text(v));
            out.push_back(std::move(line));
        }
    }
    return write_rows(out, o, eol);
}

}  // namespace flyg::data::csv
