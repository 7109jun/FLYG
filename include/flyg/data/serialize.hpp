// flyG - C++ <-> Value serialization (works with every data format) and the FLYG_FIELDS macro
// SPDX-License-Identifier: GPL-2.0-only
//
// Supported out of the box: bool, integers (range-checked), floating point, enums (as integers),
// std::string / string_view / const char*, Value, std::optional (null), std::pair / std::tuple
// (arrays), sequences (vector, deque, list, array, set, ... -> arrays), std::vector<std::byte>
// (binary), maps with string keys (objects) and maps with other keys (arrays of [key, value]).
// User types: either define `Value flyg_to_value(const T&)` and `void flyg_from_value(const Value&, T&)`
// in T's namespace (found by ADL), or list the members with FLYG_FIELDS(T, a, b, c).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <flyg/data/binary.hpp>
#include <flyg/data/json.hpp>
#include <flyg/data/toml.hpp>
#include <flyg/data/value.hpp>
#include <flyg/data/yaml.hpp>

namespace flyg::data {

template <class T>
[[nodiscard]] Value to_value(const T& v);
template <class T>
[[nodiscard]] T from_value(const Value& v);

namespace detail {

template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};
template <class T> struct is_pair : std::false_type {};
template <class A, class B> struct is_pair<std::pair<A, B>> : std::true_type {};
template <class T> struct is_tuple : std::false_type {};
template <class... A> struct is_tuple<std::tuple<A...>> : std::true_type {};
template <class T> struct is_std_array : std::false_type {};
template <class T, usize N> struct is_std_array<std::array<T, N>> : std::true_type {};

template <class T>
concept StringLike = std::convertible_to<const T&, std::string_view> && !std::same_as<std::remove_cvref_t<T>, Value>;
template <class T>
concept MapLike = requires { typename T::key_type; typename T::mapped_type; } && std::ranges::range<T>;
template <class T>
concept SetLike = requires { typename T::key_type; } && !requires { typename T::mapped_type; } && std::ranges::range<T>;
template <class T>
concept HasToValue = requires(const T& t) { { flyg_to_value(t) } -> std::convertible_to<Value>; };
template <class T>
concept HasFromValue = requires(const Value& v, T& t) { flyg_from_value(v, t); };

template <class K>
[[nodiscard]] constexpr bool key_is_string() { return std::is_convertible_v<const K&, std::string_view>; }

template <class T>
[[nodiscard]] T int_from(const Value& v) {
    if (v.is_real()) {
        const double d = v.as_double();
        if (d != std::floor(d)) throw DataException("expected an integer but found " + std::to_string(d));
    }
    if (!v.is_number()) throw DataException(std::string("type mismatch: expected integer but value is ") + std::string(to_string(v.type())));
    if (v.is_real()) {
        const double d = v.as_double();
        if (std::is_unsigned_v<T>) {
            if (d < 0 || d >= 18446744073709551616.0 || d > static_cast<double>(std::numeric_limits<T>::max())) throw DataException("integer out of range");
            return static_cast<T>(static_cast<u64>(d));
        }
    }
    const i64 i = v.as_int();
    if constexpr (std::is_unsigned_v<T>) {
        if (i < 0 || static_cast<u64>(i) > static_cast<u64>(std::numeric_limits<T>::max())) throw DataException("integer " + std::to_string(i) + " out of range");
    } else {
        if (i < static_cast<i64>(std::numeric_limits<T>::min()) || i > static_cast<i64>(std::numeric_limits<T>::max()))
            throw DataException("integer " + std::to_string(i) + " out of range");
    }
    return static_cast<T>(i);
}

template <class Tuple, usize... I>
Value tuple_to_value(const Tuple& t, std::index_sequence<I...>) {
    Array a;
    a.reserve(sizeof...(I));
    (a.push_back(to_value(std::get<I>(t))), ...);
    return Value(std::move(a));
}

template <class Tuple, usize... I>
Tuple tuple_from_value(const Value& v, std::index_sequence<I...>) {
    const auto& a = v.as_array();
    if (a.size() != sizeof...(I)) throw DataException("expected an array of " + std::to_string(sizeof...(I)) + " elements but found " + std::to_string(a.size()));
    return Tuple{from_value<std::tuple_element_t<I, Tuple>>(a[I])...};
}

}  // namespace detail

template <class T>
[[nodiscard]] Value to_value(const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, Value>) {
        return v;
    } else if constexpr (detail::HasToValue<U>) {
        return flyg_to_value(v);
    } else if constexpr (detail::is_optional<U>::value) {
        return v ? to_value(*v) : Value();
    } else if constexpr (std::is_same_v<U, bool>) {
        return Value(v);
    } else if constexpr (std::is_enum_v<U>) {
        return Value(static_cast<i64>(static_cast<std::underlying_type_t<U>>(v)));
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_unsigned_v<U>) {
            if (static_cast<u64>(v) > static_cast<u64>(std::numeric_limits<i64>::max())) throw DataException("unsigned value does not fit into a signed 64-bit integer");
        }
        return Value(static_cast<i64>(v));
    } else if constexpr (std::is_floating_point_v<U>) {
        return Value(static_cast<double>(v));
    } else if constexpr (detail::StringLike<U>) {
        return Value(std::string(std::string_view(v)));
    } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
        return Value(Bytes(v));
    } else if constexpr (detail::is_pair<U>::value) {
        return Value::array({to_value(v.first), to_value(v.second)});
    } else if constexpr (detail::is_tuple<U>::value) {
        return detail::tuple_to_value(v, std::make_index_sequence<std::tuple_size_v<U>>{});
    } else if constexpr (detail::MapLike<U>) {
        if constexpr (detail::key_is_string<typename U::key_type>()) {
            Object o;
            for (const auto& [k, x] : v) o.insert_or_assign(std::string(std::string_view(k)), to_value(x));
            return Value(std::move(o));
        } else {
            Array a;
            for (const auto& [k, x] : v) a.push_back(Value::array({to_value(k), to_value(x)}));
            return Value(std::move(a));
        }
    } else if constexpr (std::ranges::range<U>) {
        Array a;
        for (const auto& x : v) a.push_back(to_value(x));
        return Value(std::move(a));
    } else {
        static_assert(sizeof(U) == 0, "flyG: type is not serializable; add FLYG_FIELDS or flyg_to_value/flyg_from_value");
    }
}

template <class T>
[[nodiscard]] T from_value(const Value& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, Value>) {
        return v;
    } else if constexpr (detail::HasFromValue<U>) {
        U t{};
        flyg_from_value(v, t);
        return t;
    } else if constexpr (detail::is_optional<U>::value) {
        if (v.is_null()) return U{};
        return U(from_value<typename U::value_type>(v));
    } else if constexpr (std::is_same_v<U, bool>) {
        return v.as_bool();
    } else if constexpr (std::is_enum_v<U>) {
        return static_cast<U>(detail::int_from<std::underlying_type_t<U>>(v));
    } else if constexpr (std::is_integral_v<U>) {
        return detail::int_from<U>(v);
    } else if constexpr (std::is_floating_point_v<U>) {
        return static_cast<U>(v.as_double());
    } else if constexpr (std::is_same_v<U, std::string>) {
        return v.as_string();
    } else if constexpr (std::is_same_v<U, std::vector<std::byte>>) {
        if (v.is_string()) return base64_decode(v.as_string());  // text formats store binary as base64
        return v.as_binary();
    } else if constexpr (detail::is_pair<U>::value) {
        const auto& a = v.as_array();
        if (a.size() != 2) throw DataException("expected an array of 2 elements");
        return U(from_value<typename U::first_type>(a[0]), from_value<typename U::second_type>(a[1]));
    } else if constexpr (detail::is_tuple<U>::value) {
        return detail::tuple_from_value<U>(v, std::make_index_sequence<std::tuple_size_v<U>>{});
    } else if constexpr (detail::MapLike<U>) {
        U m;
        if constexpr (std::is_same_v<typename U::key_type, std::string>) {
            for (const auto& [k, x] : v.as_object()) m.emplace(k, from_value<typename U::mapped_type>(x));
        } else {
            for (const auto& e : v.as_array()) {
                const auto& kv = e.as_array();
                if (kv.size() != 2) throw DataException("expected [key, value] pairs");
                m.emplace(from_value<typename U::key_type>(kv[0]), from_value<typename U::mapped_type>(kv[1]));
            }
        }
        return m;
    } else if constexpr (detail::is_std_array<U>::value) {
        const auto& a = v.as_array();
        constexpr usize N = std::tuple_size_v<U>;
        if (a.size() != N) throw DataException("expected an array of " + std::to_string(N) + " elements but found " + std::to_string(a.size()));
        U out{};
        for (usize i = 0; i < N; ++i) out[i] = from_value<typename U::value_type>(a[i]);
        return out;
    } else if constexpr (std::ranges::range<U> && requires { typename U::value_type; }) {
        U out;
        for (const auto& x : v.as_array()) {
            if constexpr (requires { out.push_back(from_value<typename U::value_type>(x)); }) out.push_back(from_value<typename U::value_type>(x));
            else out.insert(from_value<typename U::value_type>(x));
        }
        return out;
    } else {
        static_assert(sizeof(U) == 0, "flyG: type is not deserializable; add FLYG_FIELDS or flyg_to_value/flyg_from_value");
    }
}

// ---- member-list reflection macro -----------------------------------------------------------------
namespace detail {
template <class T>
void field_to(Object& o, const char* name, const T& member) {
    o.insert_or_assign(name, to_value(member));
}
template <class T>
void field_from(const Object& o, const char* name, T& member) {
    const Value* x = o.find(name);
    if constexpr (is_optional<T>::value) {
        member = x ? from_value<T>(*x) : T{};
    } else {
        if (!x) throw DataException(std::string("missing field '") + name + "'");
        try {
            member = from_value<T>(*x);
        } catch (const DataException& e) {
            throw DataException(std::string("field '") + name + "': " + e.message());
        }
    }
}
}  // namespace detail

}  // namespace flyg::data

#define FLYG_DETAIL_EXPAND(x) x
#define FLYG_DETAIL_FOR_1(M, t, a) M(t, a)
#define FLYG_DETAIL_FOR_2(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_1(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_3(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_2(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_4(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_3(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_5(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_4(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_6(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_5(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_7(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_6(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_8(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_7(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_9(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_8(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_10(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_9(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_11(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_10(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_12(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_11(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_13(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_12(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_14(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_13(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_15(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_14(M, t, __VA_ARGS__))
#define FLYG_DETAIL_FOR_16(M, t, a, ...) M(t, a) FLYG_DETAIL_EXPAND(FLYG_DETAIL_FOR_15(M, t, __VA_ARGS__))
#define FLYG_DETAIL_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, NAME, ...) NAME
#define FLYG_DETAIL_FOR_EACH(M, t, ...)                                                                                     \
    FLYG_DETAIL_EXPAND(FLYG_DETAIL_PICK(__VA_ARGS__, FLYG_DETAIL_FOR_16, FLYG_DETAIL_FOR_15, FLYG_DETAIL_FOR_14,            \
                                        FLYG_DETAIL_FOR_13, FLYG_DETAIL_FOR_12, FLYG_DETAIL_FOR_11, FLYG_DETAIL_FOR_10,     \
                                        FLYG_DETAIL_FOR_9, FLYG_DETAIL_FOR_8, FLYG_DETAIL_FOR_7, FLYG_DETAIL_FOR_6,         \
                                        FLYG_DETAIL_FOR_5, FLYG_DETAIL_FOR_4, FLYG_DETAIL_FOR_3, FLYG_DETAIL_FOR_2,         \
                                        FLYG_DETAIL_FOR_1)(M, t, __VA_ARGS__))
#define FLYG_DETAIL_TO(t, f) ::flyg::data::detail::field_to(flyg_obj_, #f, flyg_val_.f);
#define FLYG_DETAIL_FROM(t, f) ::flyg::data::detail::field_from(flyg_obj_, #f, flyg_val_.f);

/// Declares (at namespace scope, in the namespace of `Type`) JSON/YAML/TOML/binary serialization
/// of up to 16 data members:   FLYG_FIELDS(Point, x, y)
#define FLYG_FIELDS(Type, ...)                                                                                              \
    inline ::flyg::data::Value flyg_to_value(const Type& flyg_val_) {                                                       \
        ::flyg::data::Object flyg_obj_;                                                                                     \
        FLYG_DETAIL_FOR_EACH(FLYG_DETAIL_TO, Type, __VA_ARGS__)                                                             \
        return ::flyg::data::Value(std::move(flyg_obj_));                                                                   \
    }                                                                                                                       \
    inline void flyg_from_value(const ::flyg::data::Value& flyg_v_, Type& flyg_val_) {                                      \
        const ::flyg::data::Object& flyg_obj_ = flyg_v_.as_object();                                                        \
        FLYG_DETAIL_FOR_EACH(FLYG_DETAIL_FROM, Type, __VA_ARGS__)                                                           \
    }

namespace flyg::data {

enum class Format { json, yaml, toml, binary };

/// Serializes any supported C++ value to text (json/yaml/toml) or, for Format::binary, to an FBIN
/// container held in a std::string.
template <class T>
[[nodiscard]] std::string serialize(const T& v, Format f = Format::json) {
    const Value val = to_value(v);
    switch (f) {
        case Format::json: return json::stringify(val);
        case Format::yaml: return yaml::emit(val);
        case Format::toml: return toml::write(val);
        case Format::binary: {
            const auto b = binary::encode(val);
            return std::string(reinterpret_cast<const char*>(b.data()), b.size());
        }
    }
    throw DataException("unknown format");
}

template <class T>
[[nodiscard]] T deserialize(std::string_view text, Format f = Format::json) {
    switch (f) {
        case Format::json: return from_value<T>(json::parse(text));
        case Format::yaml: return from_value<T>(yaml::parse(text));
        case Format::toml: return from_value<T>(toml::parse(text));
        case Format::binary: return from_value<T>(binary::decode(std::as_bytes(std::span(text.data(), text.size()))));
    }
    throw DataException("unknown format");
}

}  // namespace flyg::data
