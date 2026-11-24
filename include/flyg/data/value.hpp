// flyG - dynamic value model shared by all data formats (JSON, YAML, TOML, XML bridge, binary...)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <flyg/core/exception.hpp>
#include <flyg/core/types.hpp>

namespace flyg::data {

using core::i64;
using core::u64;
using core::usize;

class Value;
using Array = std::vector<Value>;
using Bytes = std::vector<std::byte>;

enum class Type : core::u8 { null, boolean, integer, real, string, binary, array, object };

[[nodiscard]] constexpr std::string_view to_string(Type t) noexcept {
    switch (t) {
        case Type::null: return "null";
        case Type::boolean: return "boolean";
        case Type::integer: return "integer";
        case Type::real: return "real";
        case Type::string: return "string";
        case Type::binary: return "binary";
        case Type::array: return "array";
        case Type::object: return "object";
    }
    return "?";
}

/// Insertion-ordered string-keyed map. Lookups are O(1) through a lazily built hash index once
/// the object has more than a handful of members; erase is O(n).
class Object {
public:
    using Entry = std::pair<std::string, Value>;
    using iterator = std::vector<Entry>::iterator;
    using const_iterator = std::vector<Entry>::const_iterator;

    Object() = default;
    Object(std::initializer_list<Entry> init);

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    iterator begin() noexcept;
    iterator end() noexcept;
    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;
    void clear() noexcept;
    void reserve(usize n);

    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] Value* find(std::string_view key);
    [[nodiscard]] bool contains(std::string_view key) const { return find(key) != nullptr; }
    [[nodiscard]] const Value& at(std::string_view key) const;
    [[nodiscard]] Value& at(std::string_view key);
    /// Inserts a null member when the key is missing.
    Value& operator[](std::string_view key);
    /// Inserts or replaces; returns the stored value.
    Value& insert_or_assign(std::string key, Value v);
    /// Appends without checking for duplicates (caller guarantees uniqueness).
    Value& append_unchecked(std::string key, Value v);
    bool erase(std::string_view key);
    [[nodiscard]] std::vector<std::string> keys() const;

private:
    static constexpr usize index_threshold = 12;
    [[nodiscard]] usize locate(std::string_view key) const;
    void build_index() const;

    std::vector<Entry> items_;
    mutable std::unordered_map<std::string, usize> index_;
    mutable bool indexed_ = false;
};

class Value {
public:
    using Storage = std::variant<std::monostate, bool, i64, double, std::string, Bytes, Array, Object>;

    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool b) noexcept : v_(b) {}
    template <std::integral I>
        requires(!std::same_as<I, bool>)
    Value(I i) noexcept {
        if constexpr (std::is_unsigned_v<I>) {
            if (static_cast<u64>(i) > static_cast<u64>(std::numeric_limits<i64>::max())) v_ = static_cast<double>(i);
            else v_ = static_cast<i64>(i);
        } else {
            v_ = static_cast<i64>(i);
        }
    }
    template <std::floating_point F>
    Value(F f) noexcept : v_(static_cast<double>(f)) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) noexcept : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Bytes b) noexcept : v_(std::move(b)) {}
    Value(Array a) noexcept : v_(std::move(a)) {}
    Value(Object o) noexcept : v_(std::move(o)) {}

    [[nodiscard]] static Value array(std::initializer_list<Value> items = {}) { return Value(Array(items)); }
    [[nodiscard]] static Value object(std::initializer_list<Object::Entry> items = {}) {
        return Value(Object(items));
    }

    [[nodiscard]] Type type() const noexcept { return static_cast<Type>(v_.index() == 0 ? 0 : v_.index()); }
    [[nodiscard]] bool is_null() const noexcept { return v_.index() == 0; }
    [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(v_); }
    [[nodiscard]] bool is_integer() const noexcept { return std::holds_alternative<i64>(v_); }
    [[nodiscard]] bool is_real() const noexcept { return std::holds_alternative<double>(v_); }
    [[nodiscard]] bool is_number() const noexcept { return is_integer() || is_real(); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(v_); }
    [[nodiscard]] bool is_binary() const noexcept { return std::holds_alternative<Bytes>(v_); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(v_); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(v_); }

    [[nodiscard]] bool as_bool() const { return get<bool>("boolean"); }
    /// Integers as-is; reals only when they hold an exact integral value in range.
    [[nodiscard]] i64 as_int() const {
        if (is_integer()) return std::get<i64>(v_);
        if (is_real()) {
            const double d = std::get<double>(v_);
            if (d == std::floor(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0) return static_cast<i64>(d);
            throw DataException("real value " + std::to_string(d) + " is not an exact integer");
        }
        throw mismatch("integer");
    }
    [[nodiscard]] double as_double() const {
        if (is_real()) return std::get<double>(v_);
        if (is_integer()) return static_cast<double>(std::get<i64>(v_));
        throw mismatch("number");
    }
    [[nodiscard]] const std::string& as_string() const { return get<std::string>("string"); }
    [[nodiscard]] std::string& as_string() { return get<std::string>("string"); }
    [[nodiscard]] const Bytes& as_binary() const { return get<Bytes>("binary"); }
    [[nodiscard]] Bytes& as_binary() { return get<Bytes>("binary"); }
    [[nodiscard]] const Array& as_array() const { return get<Array>("array"); }
    [[nodiscard]] Array& as_array() { return get<Array>("array"); }
    [[nodiscard]] const Object& as_object() const { return get<Object>("object"); }
    [[nodiscard]] Object& as_object() { return get<Object>("object"); }

    [[nodiscard]] bool bool_or(bool d) const noexcept { return is_bool() ? std::get<bool>(v_) : d; }
    [[nodiscard]] i64 int_or(i64 d) const noexcept {
        if (is_integer()) return std::get<i64>(v_);
        return d;
    }
    [[nodiscard]] double double_or(double d) const noexcept { return is_number() ? as_double() : d; }
    [[nodiscard]] std::string string_or(std::string d) const { return is_string() ? std::get<std::string>(v_) : std::move(d); }

    /// Elements of an array / members of an object / bytes of a binary / characters of a string.
    [[nodiscard]] usize size() const {
        switch (type()) {
            case Type::array: return std::get<Array>(v_).size();
            case Type::object: return std::get<Object>(v_).size();
            case Type::string: return std::get<std::string>(v_).size();
            case Type::binary: return std::get<Bytes>(v_).size();
            case Type::null: return 0;
            default: throw mismatch("container");
        }
    }
    [[nodiscard]] bool empty() const { return size() == 0; }

    // ---- object access -----------------------------------------------------------------------
    /// Mutable member access; a null value silently becomes an object.
    Value& operator[](std::string_view key) {
        if (is_null()) v_ = Object{};
        return as_object()[key];
    }
    [[nodiscard]] const Value& at(std::string_view key) const { return as_object().at(key); }
    [[nodiscard]] const Value* find(std::string_view key) const {
        return is_object() ? std::get<Object>(v_).find(key) : nullptr;
    }
    [[nodiscard]] Value* find(std::string_view key) { return is_object() ? std::get<Object>(v_).find(key) : nullptr; }
    [[nodiscard]] bool contains(std::string_view key) const { return find(key) != nullptr; }

    // ---- array access ------------------------------------------------------------------------
    [[nodiscard]] const Value& at(usize i) const {
        const auto& a = as_array();
        if (i >= a.size()) throw DataException("array index " + std::to_string(i) + " out of range (size " + std::to_string(a.size()) + ")");
        return a[i];
    }
    [[nodiscard]] Value& at(usize i) {
        auto& a = as_array();
        if (i >= a.size()) throw DataException("array index " + std::to_string(i) + " out of range (size " + std::to_string(a.size()) + ")");
        return a[i];
    }
    /// Mutable element access; a null value silently becomes an array, and the array grows with
    /// nulls so that index `i` exists.
    Value& operator[](usize i) {
        if (is_null()) v_ = Array{};
        auto& a = as_array();
        if (i >= a.size()) a.resize(i + 1);
        return a[i];
    }
    Value& push_back(Value v) {
        if (is_null()) v_ = Array{};
        auto& a = as_array();
        a.push_back(std::move(v));
        return a.back();
    }

    // ---- paths -------------------------------------------------------------------------------
    /// Looks up "a.b[2].c" style paths (dots for members, [n] for array indices; a member name
    /// containing special characters can be written as ["name"]). Returns nullptr when absent.
    [[nodiscard]] const Value* find_path(std::string_view path) const;
    /// Like find_path but creates missing containers on the way and returns the target slot.
    Value& ensure_path(std::string_view path);
    [[nodiscard]] const Value& at_path(std::string_view path) const {
        const Value* v = find_path(path);
        if (!v) throw DataException("path not found: " + std::string(path));
        return *v;
    }

    // ---- ordering ----------------------------------------------------------------------------
    /// Total order across all values: null < boolean < number < string < binary < array < object.
    /// Integers and reals compare numerically; NaN equals NaN and sorts below every other number.
    /// Objects compare as their (key, value) lists sorted by key (so member order is irrelevant).
    [[nodiscard]] static std::strong_ordering compare(const Value& a, const Value& b);
    friend bool operator==(const Value& a, const Value& b) { return compare(a, b) == 0; }
    friend std::strong_ordering operator<=>(const Value& a, const Value& b) { return compare(a, b); }

    [[nodiscard]] const Storage& storage() const noexcept { return v_; }

private:
    template <class T>
    const T& get(const char* want) const {
        if (const T* p = std::get_if<T>(&v_)) return *p;
        throw mismatch(want);
    }
    template <class T>
    T& get(const char* want) {
        if (T* p = std::get_if<T>(&v_)) return *p;
        throw mismatch(want);
    }
    [[nodiscard]] DataException mismatch(const char* want) const {
        return DataException(std::string("type mismatch: expected ") + want + " but value is " + std::string(to_string(type())));
    }

    Storage v_;
};

// ---- Object member functions (need the complete Value) -----------------------------------------
inline usize Object::size() const noexcept { return items_.size(); }
inline bool Object::empty() const noexcept { return items_.empty(); }
inline Object::iterator Object::begin() noexcept { return items_.begin(); }
inline Object::iterator Object::end() noexcept { return items_.end(); }
inline Object::const_iterator Object::begin() const noexcept { return items_.begin(); }
inline Object::const_iterator Object::end() const noexcept { return items_.end(); }
inline void Object::clear() noexcept {
    items_.clear();
    index_.clear();
    indexed_ = false;
}
inline void Object::reserve(usize n) { items_.reserve(n); }

inline Object::Object(std::initializer_list<Entry> init) {
    for (const auto& e : init) insert_or_assign(e.first, e.second);
}

inline std::vector<std::string> Object::keys() const {
    std::vector<std::string> k;
    k.reserve(items_.size());
    for (const auto& e : items_) k.push_back(e.first);
    return k;
}

inline void Object::build_index() const {
    index_.clear();
    index_.reserve(items_.size() * 2);
    for (usize i = 0; i < items_.size(); ++i) index_.emplace(items_[i].first, i);
    indexed_ = true;
}

inline usize Object::locate(std::string_view key) const {
    constexpr usize npos = static_cast<usize>(-1);
    if (items_.size() <= index_threshold) {
        for (usize i = 0; i < items_.size(); ++i)
            if (items_[i].first == key) return i;
        return npos;
    }
    if (!indexed_) build_index();
    const auto it = index_.find(std::string(key));
    return it == index_.end() ? npos : it->second;
}

inline const Value* Object::find(std::string_view key) const {
    const usize i = locate(key);
    return i == static_cast<usize>(-1) ? nullptr : &items_[i].second;
}
inline Value* Object::find(std::string_view key) {
    const usize i = locate(key);
    return i == static_cast<usize>(-1) ? nullptr : &items_[i].second;
}
inline const Value& Object::at(std::string_view key) const {
    if (const Value* v = find(key)) return *v;
    throw DataException("missing object member '" + std::string(key) + "'");
}
inline Value& Object::at(std::string_view key) {
    if (Value* v = find(key)) return *v;
    throw DataException("missing object member '" + std::string(key) + "'");
}
inline Value& Object::operator[](std::string_view key) {
    if (Value* v = find(key)) return *v;
    return append_unchecked(std::string(key), Value());
}
inline Value& Object::insert_or_assign(std::string key, Value v) {
    if (Value* e = find(key)) {
        *e = std::move(v);
        return *e;
    }
    return append_unchecked(std::move(key), std::move(v));
}
inline Value& Object::append_unchecked(std::string key, Value v) {
    items_.emplace_back(std::move(key), std::move(v));
    if (indexed_) index_[items_.back().first] = items_.size() - 1;
    return items_.back().second;
}
inline bool Object::erase(std::string_view key) {
    const usize i = locate(key);
    if (i == static_cast<usize>(-1)) return false;
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
    indexed_ = false;
    index_.clear();
    return true;
}

// ---- paths ---------------------------------------------------------------------------------------
namespace detail {
struct PathStep {
    bool is_index = false;
    usize index = 0;
    std::string key;
};

[[nodiscard]] inline std::vector<PathStep> parse_path(std::string_view p) {
    std::vector<PathStep> steps;
    usize i = 0;
    auto fail = [&](const char* why) { throw DataException(std::string("bad path '") + std::string(p) + "': " + why, i); };
    while (i < p.size()) {
        if (p[i] == '.') {
            ++i;
            if (i >= p.size() || p[i] == '.' || p[i] == '[') fail("empty member name");
        }
        if (p[i] == '[') {
            ++i;
            if (i < p.size() && p[i] == '"') {
                ++i;
                std::string key;
                while (i < p.size() && p[i] != '"') {
                    if (p[i] == '\\' && i + 1 < p.size()) ++i;
                    key += p[i++];
                }
                if (i >= p.size()) fail("unterminated quoted key");
                ++i;  // closing quote
                if (i >= p.size() || p[i] != ']') fail("expected ']'");
                ++i;
                steps.push_back({false, 0, std::move(key)});
            } else {
                usize n = 0;
                const usize start = i;
                while (i < p.size() && p[i] >= '0' && p[i] <= '9') n = n * 10 + static_cast<usize>(p[i++] - '0');
                if (i == start || i >= p.size() || p[i] != ']') fail("expected an array index");
                ++i;
                steps.push_back({true, n, {}});
            }
        } else {
            std::string key;
            while (i < p.size() && p[i] != '.' && p[i] != '[') key += p[i++];
            steps.push_back({false, 0, std::move(key)});
        }
    }
    return steps;
}
}  // namespace detail

inline const Value* Value::find_path(std::string_view path) const {
    const Value* cur = this;
    for (const auto& s : detail::parse_path(path)) {
        if (s.is_index) {
            if (!cur->is_array() || s.index >= cur->as_array().size()) return nullptr;
            cur = &cur->as_array()[s.index];
        } else {
            cur = cur->find(s.key);
            if (!cur) return nullptr;
        }
    }
    return cur;
}

inline Value& Value::ensure_path(std::string_view path) {
    Value* cur = this;
    for (const auto& s : detail::parse_path(path)) cur = s.is_index ? &(*cur)[s.index] : &(*cur)[s.key];
    return *cur;
}

// ---- ordering ------------------------------------------------------------------------------------
namespace detail {
[[nodiscard]] inline int type_rank(Type t) noexcept {
    switch (t) {
        case Type::null: return 0;
        case Type::boolean: return 1;
        case Type::integer:
        case Type::real: return 2;
        case Type::string: return 3;
        case Type::binary: return 4;
        case Type::array: return 5;
        case Type::object: return 6;
    }
    return 7;
}

[[nodiscard]] inline std::strong_ordering compare_numbers(const Value& a, const Value& b) {
    if (a.is_integer() && b.is_integer()) return a.as_int() <=> b.as_int();
    // Mixed or real: compare exactly where possible.
    auto rank = [](double d) { return std::isnan(d) ? 0 : 1; };
    if (a.is_real() && b.is_real()) {
        const double x = a.as_double(), y = b.as_double();
        if (std::isnan(x) || std::isnan(y)) return rank(x) <=> rank(y);
        if (x < y) return std::strong_ordering::less;
        if (x > y) return std::strong_ordering::greater;
        return std::strong_ordering::equal;
    }
    const i64 i = a.is_integer() ? a.as_int() : b.as_int();
    const double d = a.is_real() ? a.as_double() : b.as_double();
    std::strong_ordering r = std::strong_ordering::equal;  // ordering of (integer side) relative to (real side)
    if (std::isnan(d)) r = std::strong_ordering::greater;
    else if (d >= 9223372036854775808.0) r = std::strong_ordering::less;
    else if (d < -9223372036854775808.0) r = std::strong_ordering::greater;
    else {
        const double fl = std::floor(d);
        const i64 fi = static_cast<i64>(fl);
        if (i != fi) r = i <=> fi;
        else r = (d > fl) ? std::strong_ordering::less : std::strong_ordering::equal;
    }
    if (a.is_integer()) return r;
    if (r == std::strong_ordering::less) return std::strong_ordering::greater;
    if (r == std::strong_ordering::greater) return std::strong_ordering::less;
    return std::strong_ordering::equal;
}
}  // namespace detail

inline std::strong_ordering Value::compare(const Value& a, const Value& b) {
    const int ra = detail::type_rank(a.type()), rb = detail::type_rank(b.type());
    if (ra != rb) return ra <=> rb;
    switch (a.type()) {
        case Type::null: return std::strong_ordering::equal;
        case Type::boolean: return a.as_bool() <=> b.as_bool();
        case Type::integer:
        case Type::real: return detail::compare_numbers(a, b);
        case Type::string: {
            const int c = a.as_string().compare(b.as_string());
            return c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
        }
        case Type::binary: {
            const auto& x = a.as_binary();
            const auto& y = b.as_binary();
            const usize n = std::min(x.size(), y.size());
            for (usize i = 0; i < n; ++i)
                if (x[i] != y[i]) return static_cast<unsigned>(x[i]) <=> static_cast<unsigned>(y[i]);
            return x.size() <=> y.size();
        }
        case Type::array: {
            const auto& x = a.as_array();
            const auto& y = b.as_array();
            const usize n = std::min(x.size(), y.size());
            for (usize i = 0; i < n; ++i)
                if (auto c = compare(x[i], y[i]); c != 0) return c;
            return x.size() <=> y.size();
        }
        case Type::object: {
            auto sorted = [](const Object& o) {
                std::vector<const Object::Entry*> v;
                v.reserve(o.size());
                for (const auto& e : o) v.push_back(&e);
                std::sort(v.begin(), v.end(), [](auto* l, auto* r) { return l->first < r->first; });
                return v;
            };
            const auto x = sorted(a.as_object());
            const auto y = sorted(b.as_object());
            const usize n = std::min(x.size(), y.size());
            for (usize i = 0; i < n; ++i) {
                if (const int c = x[i]->first.compare(y[i]->first); c != 0)
                    return c < 0 ? std::strong_ordering::less : std::strong_ordering::greater;
                if (auto c = compare(x[i]->second, y[i]->second); c != 0) return c;
            }
            return x.size() <=> y.size();
        }
    }
    return std::strong_ordering::equal;
}

}  // namespace flyg::data
