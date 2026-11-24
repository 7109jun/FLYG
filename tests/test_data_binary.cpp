// flyG - data module tests: binary format and serialization
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>

#include <cmath>
#include <unordered_map>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

// ---- user types for the serialization tests --------------------------------------------------
namespace app {
struct Point {
    double x = 0, y = 0;
    bool operator==(const Point&) const = default;
};
FLYG_FIELDS(Point, x, y)

enum class Color : unsigned char { red = 1, green = 2, blue = 3 };

struct Shape {
    std::string name;
    Color color = Color::red;
    std::vector<Point> points;
    std::optional<std::string> label;
    std::map<std::string, int> tags;
    std::pair<int, std::string> pair{};
    std::tuple<int, double, std::string> tup{};
    std::array<int, 3> arr{};
    std::vector<std::byte> blob;
    std::map<int, std::string> int_keys;
    std::set<std::string> uniq;
    bool operator==(const Shape&) const = default;
};
FLYG_FIELDS(Shape, name, color, points, label, tags, pair, tup, arr, blob, int_keys, uniq)


struct Settings2 {
    std::string name;
    std::optional<std::string> nick;
};
FLYG_FIELDS(Settings2, name, nick)

// custom (non-macro) serialization through ADL
struct Temperature {
    double celsius = 0;
    bool operator==(const Temperature&) const = default;
};
inline Value flyg_to_value(const Temperature& t) { return Value(std::to_string(t.celsius) + "C"); }
inline void flyg_from_value(const Value& v, Temperature& t) { t.celsius = std::stod(v.as_string()); }

struct Settings {
    std::string name;
    int port = 0;
    double ratio = 0;
    std::vector<std::string> tags;
    Point origin;
    Temperature temp;
    bool operator==(const Settings&) const = default;
};
FLYG_FIELDS(Settings, name, port, ratio, tags, origin, temp)
}  // namespace app

// ---- ByteWriter / ByteReader -----------------------------------------------------------------
FLYG_TEST(data_bytes_primitives) {
    ByteWriter w;
    w.put_u8(0xAB);
    w.put_u16(0x1234);
    w.put_u32(0xDEADBEEF);
    w.put_u64(0x0102030405060708ull);
    w.put_i64(-2);
    w.put_f32(1.5f);
    w.put_f64(-2.25);
    w.put_string("héllo");
    const std::byte raw[] = {std::byte{1}, std::byte{2}, std::byte{3}};
    w.put_bytes(raw);
    CHECK_EQ(w.view()[1], std::byte{0x34});  // little endian
    CHECK_EQ(w.view()[2], std::byte{0x12});

    ByteReader r(w.view());
    CHECK_EQ(r.read_u8(), core::u8{0xAB});
    CHECK_EQ(r.read_u16(), core::u16{0x1234});
    CHECK_EQ(r.read_u32(), u32{0xDEADBEEF});
    CHECK_EQ(r.read_u64(), u64{0x0102030405060708ull});
    CHECK_EQ(r.read_i64(), i64{-2});
    CHECK_EQ(r.read_f32(), 1.5f);
    CHECK_EQ(r.read_f64(), -2.25);
    CHECK_EQ(r.read_string(), "héllo");
    CHECK_EQ(r.read_bytes().size(), usize{3});
    CHECK(r.at_end());
    CHECK_THROWS_AS(r.read_u8(), DataException);
}

FLYG_TEST(data_bytes_varints) {
    const u64 vals[] = {0, 1, 127, 128, 255, 16383, 16384, 0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
    for (u64 v : vals) {
        ByteWriter w;
        w.put_varint(v);
        ByteReader r(w.view());
        CHECK_EQ(r.read_varint(), v);
        CHECK(r.at_end());
    }
    ByteWriter small;
    small.put_varint(127);
    CHECK_EQ(small.size(), usize{1});
    small.put_varint(128);
    CHECK_EQ(small.size(), usize{3});
    const i64 svals[] = {0, -1, 1, -2, 63, -64, 64, std::numeric_limits<i64>::max(), std::numeric_limits<i64>::min()};
    for (i64 v : svals) {
        ByteWriter w;
        w.put_svarint(v);
        ByteReader r(w.view());
        CHECK_EQ(r.read_svarint(), v);
    }
    ByteWriter one;
    one.put_svarint(-1);
    CHECK_EQ(one.size(), usize{1});  // zig-zag keeps small negatives small

    // malformed varints
    const std::byte too_long[11] = {std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
                                    std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x01}};
    ByteReader a{std::span<const std::byte>(too_long)};
    CHECK_THROWS_AS(a.read_varint(), DataException);
    const std::byte overflow[10] = {std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
                                    std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
    ByteReader b{std::span<const std::byte>(overflow)};
    CHECK_THROWS_AS(b.read_varint(), DataException);
    const std::byte trunc[1] = {std::byte{0x80}};
    ByteReader c{std::span<const std::byte>(trunc)};
    CHECK_THROWS_AS(c.read_varint(), DataException);
    // string length larger than the buffer
    ByteWriter lie;
    lie.put_varint(1000);
    lie.put_u8('x');
    ByteReader d(lie.view());
    CHECK_THROWS_AS(d.read_string(), DataException);

    ByteWriter p;
    p.put_u32(0);
    p.patch_u32(0, 0x01020304);
    CHECK_EQ(p.view()[0], std::byte{4});
    CHECK_THROWS_AS(p.patch_u32(1, 0), DataException);
}

// ---- FBIN container --------------------------------------------------------------------------
FLYG_TEST(data_binary_round_trip) {
    const Value v = json::parse(R"({"s":"한글😀","n":[1,-2,3.5,null,true,false],"o":{"x":{"y":[[],{}]}},"big":9007199254740993,"neg":-9223372036854775808})");
    const auto enc = binary::encode(v);
    CHECK(binary::decode(enc) == v);
    CHECK(enc.size() < json::stringify(v).size());
    Value withbin = v;
    withbin["blob"] = Bytes{std::byte{0}, std::byte{255}};
    CHECK(binary::decode(binary::encode(withbin)) == withbin);
    CHECK(binary::decode(binary::encode(Value())).is_null());
    CHECK(binary::decode(binary::encode(Value(std::nan("")))).is_real());

    // doubles are bit exact
    for (double d : {0.1, -0.0, 1e-300, 5e-324, 1.7976931348623157e308}) {
        const double back = binary::decode(binary::encode(Value(d))).as_double();
        CHECK(std::memcmp(&back, &d, sizeof d) == 0);
    }
    // key order is preserved
    const Value ordered = binary::decode(binary::encode(json::parse(R"({"z":1,"a":2,"m":3})")));
    std::string order;
    for (const auto& [k, x] : ordered.as_object()) order += k;
    CHECK_EQ(order, "zam");

    u64 seed = 99;
    auto rnd = [&] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(seed >> 33); };
    std::function<Value(int)> gen = [&](int depth) -> Value {
        switch (rnd() % (depth > 3 ? 6 : 8)) {
            case 0: return Value();
            case 1: return Value(rnd() % 2 == 0);
            case 2: return Value(static_cast<i64>(rnd()) * (rnd() % 2 ? 1 : -1000000));
            case 3: return Value(static_cast<double>(rnd()) / 3.0);
            case 4: return Value(std::string(rnd() % 20, static_cast<char>('a' + rnd() % 26)));
            case 5: return Value(Bytes(rnd() % 10, static_cast<std::byte>(rnd())));
            case 6: { Array a; for (u32 i = rnd() % 5; i > 0; --i) a.push_back(gen(depth + 1)); return Value(a); }
            default: { Object o; for (u32 i = rnd() % 5; i > 0; --i) o.insert_or_assign("k" + std::to_string(rnd() % 30), gen(depth + 1)); return Value(o); }
        }
    };
    for (int i = 0; i < 300; ++i) {
        const Value g = gen(0);
        CHECK(binary::decode(binary::encode(g)) == g);
    }
}

FLYG_TEST(data_binary_corruption) {
    const Value v = json::parse(R"({"name":"flyG","list":[1,2,3],"nested":{"a":"b"}})");
    const auto good = binary::encode(v);
    CHECK(binary::decode(good) == v);

    // every single-byte flip must be detected (CRC), never crash
    for (usize i = 0; i < good.size(); ++i) {
        auto bad = good;
        bad[i] = static_cast<std::byte>(static_cast<unsigned>(bad[i]) ^ 0x5A);
        bool threw = false;
        try {
            (void)binary::decode(bad);
        } catch (const DataException&) {
            threw = true;
        }
        if (!threw) flyg_test::report_failure(__FILE__, __LINE__, "corruption at byte " + std::to_string(i) + " not detected");
        ++flyg_test::check_count();
    }
    // truncation at every length
    for (usize n = 0; n < good.size(); ++n) CHECK_THROWS_AS(binary::decode(std::span<const std::byte>(good.data(), n)), DataException);
    // trailing garbage
    auto extra = good;
    extra.push_back(std::byte{0});
    CHECK_THROWS_AS(binary::decode(extra), DataException);
    CHECK_THROWS_AS(binary::decode({}), DataException);

    // hostile raw payloads: absurd lengths must not allocate or crash
    {
        ByteWriter w;
        w.put_u8(binary::tag::array);
        w.put_varint(0xFFFFFFFFFFFFull);
        ByteReader r(w.view());
        CHECK_THROWS_AS(binary::decode_raw(r), DataException);
    }
    {
        ByteWriter w;
        w.put_u8(binary::tag::object);
        w.put_varint(1ull << 40);
        ByteReader r(w.view());
        CHECK_THROWS_AS(binary::decode_raw(r), DataException);
    }
    {
        ByteWriter w;
        for (int i = 0; i < 100000; ++i) {
            w.put_u8(binary::tag::array);
            w.put_varint(1);
        }
        ByteReader r(w.view());
        CHECK_THROWS_AS(binary::decode_raw(r), DataException);  // depth limit
    }
    {
        ByteWriter w;
        w.put_u8(binary::tag::object);
        w.put_varint(2);
        w.put_string("k");
        w.put_u8(binary::tag::null);
        w.put_string("k");
        w.put_u8(binary::tag::null);
        ByteReader r(w.view());
        CHECK_THROWS_AS(binary::decode_raw(r), DataException);  // duplicate key
    }
    {
        const std::byte bad_tag[1] = {std::byte{200}};
        ByteReader r{std::span<const std::byte>(bad_tag)};
        CHECK_THROWS_AS(binary::decode_raw(r), DataException);
    }
}

// ---- serialization ---------------------------------------------------------------------------
FLYG_TEST(data_serialize_builtin) {
    CHECK_EQ(json::stringify(to_value(42)), "42");
    CHECK_EQ(json::stringify(to_value(std::string("x"))), "\"x\"");
    CHECK_EQ(json::stringify(to_value("literal")), "\"literal\"");
    CHECK_EQ(json::stringify(to_value(std::string_view("sv"))), "\"sv\"");
    CHECK_EQ(json::stringify(to_value(std::vector<int>{1, 2, 3})), "[1,2,3]");
    CHECK_EQ(json::stringify(to_value(std::map<std::string, double>{{"a", 1.5}, {"b", 2.0}})), R"({"a":1.5,"b":2.0})");
    CHECK_EQ(json::stringify(to_value(std::optional<int>())), "null");
    CHECK_EQ(json::stringify(to_value(std::optional<int>(5))), "5");
    CHECK_EQ(json::stringify(to_value(std::make_pair(1, std::string("a")))), R"([1,"a"])");
    CHECK_EQ(json::stringify(to_value(std::make_tuple(1, 2.5, std::string("t")))), R"([1,2.5,"t"])");
    CHECK_EQ(json::stringify(to_value(std::map<int, std::string>{{1, "a"}, {2, "b"}})), R"([[1,"a"],[2,"b"]])");
    CHECK_EQ(json::stringify(to_value(std::set<int>{3, 1, 2})), "[1,2,3]");
    CHECK_EQ(json::stringify(to_value(std::array<int, 2>{7, 8})), "[7,8]");
    CHECK_EQ(json::stringify(to_value(std::vector<std::vector<int>>{{1}, {}, {2, 3}})), "[[1],[],[2,3]]");
    CHECK_EQ(json::stringify(to_value(app::Color::blue)), "3");
    CHECK(to_value(std::vector<std::byte>{std::byte{1}}).is_binary());
    CHECK_THROWS_AS(to_value(std::numeric_limits<u64>::max()), DataException);

    CHECK_EQ(from_value<int>(Value(42)), 42);
    CHECK_EQ(from_value<double>(Value(3)), 3.0);
    CHECK_EQ(from_value<float>(Value(1.5)), 1.5f);
    CHECK_EQ(from_value<unsigned char>(Value(255)), static_cast<unsigned char>(255));
    CHECK_EQ(from_value<i64>(Value(3.0)), i64{3});
    CHECK_THROWS_AS(from_value<unsigned char>(Value(256)), DataException);
    CHECK_THROWS_AS(from_value<unsigned>(Value(-1)), DataException);
    CHECK_THROWS_AS(from_value<int8_t>(Value(-129)), DataException);
    CHECK_THROWS_AS(from_value<int>(Value(1.5)), DataException);
    CHECK_THROWS_AS(from_value<int>(Value("x")), DataException);
    CHECK_THROWS_AS(from_value<std::string>(Value(1)), DataException);
    CHECK_THROWS_AS(from_value<bool>(Value(1)), DataException);
    CHECK(from_value<bool>(Value(true)));
    CHECK((from_value<std::vector<int>>(json::parse("[1,2,3]")) == std::vector<int>{1, 2, 3}));
    CHECK_THROWS_AS(from_value<std::vector<int>>(json::parse("[1,\"x\"]")), DataException);
    CHECK((from_value<std::map<std::string, int>>(json::parse(R"({"a":1})")) == std::map<std::string, int>{{"a", 1}}));
    CHECK((from_value<std::map<int, std::string>>(json::parse(R"([[1,"a"]])")) == std::map<int, std::string>{{1, "a"}}));
    CHECK((from_value<std::unordered_map<std::string, int>>(json::parse(R"({"a":1,"b":2})")).size() == 2));
    CHECK(!from_value<std::optional<int>>(Value()).has_value());
    CHECK_EQ(*from_value<std::optional<int>>(Value(3)), 3);
    CHECK((from_value<std::pair<int, std::string>>(json::parse(R"([1,"a"])")) == std::make_pair(1, std::string("a"))));
    CHECK_THROWS_AS((from_value<std::pair<int, int>>(json::parse("[1]"))), DataException);
    CHECK((from_value<std::array<int, 2>>(json::parse("[1,2]")) == std::array<int, 2>{1, 2}));
    CHECK_THROWS_AS((from_value<std::array<int, 2>>(json::parse("[1,2,3]"))), DataException);
    CHECK(from_value<app::Color>(Value(2)) == app::Color::green);
    CHECK_EQ(from_value<std::set<int>>(json::parse("[3,1,3]")).size(), usize{2});
}

FLYG_TEST(data_serialize_user_types) {
    app::Shape s;
    s.name = "poly";
    s.color = app::Color::green;
    s.points = {{0, 0}, {1.5, -2}, {3, 4}};
    s.label = "tri";
    s.tags = {{"a", 1}, {"b", 2}};
    s.pair = {7, "seven"};
    s.tup = {1, 2.5, "three"};
    s.arr = {1, 2, 3};
    s.blob = {std::byte{0}, std::byte{0xFF}};
    s.int_keys = {{1, "one"}, {2, "two"}};
    s.uniq = {"x", "y"};

    for (Format f : {Format::json, Format::yaml, Format::binary}) {
        const std::string text = serialize(s, f);
        const app::Shape back = deserialize<app::Shape>(text, f);
        CHECK(back == s);
    }
    // TOML cannot hold binary or null: use a type without them
    app::Settings cfg;
    cfg.name = "svc";
    cfg.port = 8080;
    cfg.ratio = 0.75;
    cfg.tags = {"a", "b"};
    cfg.origin = {1, 2};
    cfg.temp = {21.5};
    for (Format f : {Format::json, Format::yaml, Format::toml, Format::binary}) CHECK(deserialize<app::Settings>(serialize(cfg, f), f) == cfg);
    CHECK_EQ(serialize(cfg, Format::json), R"({"name":"svc","port":8080,"ratio":0.75,"tags":["a","b"],"origin":{"x":1.0,"y":2.0},"temp":"21.500000C"})");
    CHECK_THROWS_AS(serialize(s, Format::toml), DataException);  // binary member
}

FLYG_TEST(data_serialize_errors) {
    // missing field
    CHECK_THROWS_AS(deserialize<app::Point>(R"({"x":1})"), DataException);
    try {
        (void)deserialize<app::Point>(R"({"x":1})");
    } catch (const DataException& e) {
        CHECK(e.message().find("missing field 'y'") != std::string::npos);
    }
    // wrong type names the field
    try {
        (void)deserialize<app::Point>(R"({"x":"a","y":2})");
        CHECK(false);
    } catch (const DataException& e) {
        CHECK(e.message().find("field 'x'") != std::string::npos);
    }
    // extra fields are ignored; optional fields may be absent
    CHECK(deserialize<app::Point>(R"({"x":1,"y":2,"extra":[1,2]})") == (app::Point{1, 2}));
    const auto lenient = deserialize<app::Settings2>(R"({"name":"n"})");
    CHECK_EQ(lenient.name, "n");
    CHECK(!lenient.nick.has_value());
    CHECK_EQ(deserialize<app::Settings2>(R"({"name":"n","nick":"m"})").nick.value(), "m");
    CHECK_THROWS_AS(deserialize<app::Point>("[1,2]"), DataException);  // not an object
    CHECK_THROWS_AS(deserialize<app::Point>("{bad json"), DataException);
}
