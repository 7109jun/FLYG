// flyG - data module tests: encoding, Value, JSON
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>

#include <cmath>
#include <cstring>
#include <sstream>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

// ---- encoding ----------------------------------------------------------------------------------
FLYG_TEST(data_utf8) {
    CHECK(utf8::is_valid("plain ascii"));
    CHECK(utf8::is_valid("한글 é 😀"));
    CHECK(!utf8::is_valid("\xC0\xAF"));          // overlong '/'
    CHECK(!utf8::is_valid("\xED\xA0\x80"));      // surrogate
    CHECK(!utf8::is_valid("\xF4\x90\x80\x80"));  // > U+10FFFF
    CHECK(!utf8::is_valid("abc\xE2\x82"));       // truncated
    CHECK(!utf8::is_valid("\x80"));
    CHECK_EQ(utf8::sanitize("a\xFF" "b"), "a\xEF\xBF\xBD" "b");
    CHECK_EQ(utf8::length("한글😀"), usize{3});
    const std::u32string u = utf8::to_utf32("a한😀");
    CHECK_EQ(u.size(), usize{3});
    CHECK_EQ(u[2], char32_t{0x1F600});
    CHECK_EQ(utf8::from_utf32(u), "a한😀");
    const std::u16string w = utf8::to_utf16("a한😀");
    CHECK_EQ(w.size(), usize{4});
    CHECK_EQ(utf8::from_utf16(w), "a한😀");
    CHECK_THROWS_AS(utf8::from_utf16(u"\xD800"), DataException);
    CHECK_THROWS_AS(utf8::from_utf16(u"\xDC00x"), DataException);
    CHECK_THROWS_AS(utf8::to_utf32("\xFF"), DataException);
    CHECK_THROWS_AS(utf8::from_utf32(U"\xD800"), DataException);
}

FLYG_TEST(data_base64_hex_percent) {
    CHECK_EQ(base64_encode("", false), "");
    CHECK_EQ(base64_encode("f"), "Zg==");
    CHECK_EQ(base64_encode("fo"), "Zm8=");
    CHECK_EQ(base64_encode("foo"), "Zm9v");
    CHECK_EQ(base64_encode("foobar"), "Zm9vYmFy");
    CHECK_EQ(base64_encode("fo", false, false), "Zm8");
    const std::string bin = "\xfb\xff\xfe";
    CHECK_EQ(base64_encode(bin), "+//+");
    CHECK_EQ(base64_encode(bin, true), "-__-");
    for (usize n = 0; n < 40; ++n) {
        std::vector<std::byte> d(n);
        for (usize i = 0; i < n; ++i) d[i] = static_cast<std::byte>(i * 37 + n);
        CHECK(base64_decode(base64_encode(d)) == d);
        CHECK(base64_decode(base64_encode(d, true, false)) == d);
    }
    CHECK_EQ(base64_decode("Zm9v\nYmFy").size(), usize{6});
    CHECK_THROWS_AS(base64_decode("Zm9v!"), DataException);
    CHECK_THROWS_AS(base64_decode("Z"), DataException);
    CHECK_THROWS_AS(base64_decode("Zg=x"), DataException);
    CHECK_THROWS_AS(base64_decode("Zg==Zg"), DataException);

    const std::vector<std::byte> bytes{std::byte{0}, std::byte{0xAB}, std::byte{0xff}};
    CHECK_EQ(hex_encode(bytes), "00abff");
    CHECK_EQ(hex_encode(bytes, true), "00ABFF");
    CHECK(hex_decode("00AbFf") == bytes);
    CHECK_THROWS_AS(hex_decode("abc"), DataException);
    CHECK_THROWS_AS(hex_decode("zz"), DataException);

    CHECK_EQ(percent_encode("a b&c=d/한"), "a%20b%26c%3Dd%2F%ED%95%9C");
    CHECK_EQ(percent_encode("a b", true), "a+b");
    CHECK_EQ(percent_decode("a%20b%26c%3Dd%2F%ED%95%9C"), "a b&c=d/한");
    CHECK_EQ(percent_decode("a+b", true), "a b");
    CHECK_EQ(percent_decode("a+b"), "a+b");
    CHECK_THROWS_AS(percent_decode("%4"), DataException);
    CHECK_THROWS_AS(percent_decode("%zz"), DataException);
    CHECK_THROWS_AS(percent_decode("abc%"), DataException);
}

FLYG_TEST(data_checksums) {
    CHECK_EQ(crc32("123456789"), u32{0xCBF43926});  // standard check value
    CHECK_EQ(crc32(""), u32{0});
    const u32 whole = crc32("hello world");
    CHECK_EQ(crc32(" world", crc32("hello")), whole);
    CHECK_EQ(fnv1a64(""), u64{14695981039346656037ull});
    CHECK_EQ(fnv1a64("a"), u64{0xaf63dc4c8601ec8cull});
}

// ---- Value -----------------------------------------------------------------------------------
FLYG_TEST(data_value_basics) {
    Value v;
    CHECK(v.is_null());
    CHECK(Value(true).as_bool());
    CHECK_EQ(Value(42).as_int(), i64{42});
    CHECK_EQ(Value(42u).type(), Type::integer);
    CHECK_EQ(Value(std::numeric_limits<u64>::max()).type(), Type::real);  // does not fit i64
    CHECK_EQ(Value(2.5f).as_double(), 2.5);
    CHECK_EQ(Value(3.0).as_int(), i64{3});
    CHECK_THROWS_AS(Value(3.5).as_int(), DataException);
    CHECK_THROWS_AS(Value(1e30).as_int(), DataException);
    CHECK_EQ(Value(7).as_double(), 7.0);
    CHECK_EQ(Value("hi").as_string(), "hi");
    CHECK_EQ(Value(std::string_view("sv")).as_string(), "sv");
    CHECK_THROWS_AS(Value(1).as_string(), DataException);
    CHECK_THROWS_AS(Value("x").as_array(), DataException);
    CHECK_EQ(Value("x").string_or("d"), "x");
    CHECK_EQ(Value(1).string_or("d"), "d");
    CHECK_EQ(Value(5).int_or(0), i64{5});
    CHECK_EQ(Value("5").int_or(9), i64{9});
    CHECK(Value(nullptr).is_null());
    CHECK_EQ(to_string(Type::binary), "binary");
    try {
        (void)Value(1).as_array();
    } catch (const DataException& e) {
        CHECK(e.message().find("expected array but value is integer") != std::string::npos);
    }
}

FLYG_TEST(data_value_containers) {
    Value v;
    v["name"] = "flyG";
    v["tags"][2] = "c";  // auto-creates, grows with nulls
    v["nested"]["deep"]["x"] = 1;
    CHECK(v.is_object());
    CHECK_EQ(v.size(), usize{3});
    CHECK_EQ(v.at("tags").size(), usize{3});
    CHECK(v.at("tags").at(0).is_null());
    CHECK_EQ(v.at("tags").at(2).as_string(), "c");
    CHECK_THROWS_AS(v.at("missing"), DataException);
    CHECK_THROWS_AS(v.at("tags").at(3), DataException);
    CHECK(v.find("name") != nullptr);
    CHECK(v.find("nope") == nullptr);
    CHECK(Value(1).find("x") == nullptr);

    Value arr;
    arr.push_back(1);
    arr.push_back("two");
    CHECK(arr.is_array());
    CHECK_EQ(arr.size(), usize{2});

    // insertion order is preserved
    Value o = Value::object({{"z", 1}, {"a", 2}, {"m", 3}});
    std::string order;
    for (const auto& [k, x] : o.as_object()) order += k;
    CHECK_EQ(order, "zam");
    CHECK(o.as_object().erase("a"));
    CHECK(!o.as_object().erase("a"));
    CHECK_EQ(o.size(), usize{2});
    CHECK_EQ(o.at("m").as_int(), i64{3});

    // large objects use the hash index; verify semantics survive erase/insert cycles
    Object big;
    for (int i = 0; i < 1000; ++i) big["k" + std::to_string(i)] = i;
    CHECK_EQ(big.at("k777").as_int(), i64{777});
    CHECK(big.erase("k500"));
    CHECK(!big.contains("k500"));
    CHECK_EQ(big.at("k999").as_int(), i64{999});
    big["k500"] = -1;
    CHECK_EQ(big.at("k500").as_int(), i64{-1});
    CHECK_EQ(big.size(), usize{1000});
    big.insert_or_assign("k1", "replaced");
    CHECK_EQ(big.at("k1").as_string(), "replaced");
    CHECK_EQ(big.size(), usize{1000});
}

FLYG_TEST(data_value_paths) {
    Value v = json::parse(R"({"a":{"b":[10,{"c":"deep"}]},"we ird":{"k.x":1}})");
    CHECK_EQ(v.at_path("a.b[0]").as_int(), i64{10});
    CHECK_EQ(v.at_path("a.b[1].c").as_string(), "deep");
    CHECK_EQ(v.at_path("[\"we ird\"][\"k.x\"]").as_int(), i64{1});
    CHECK(v.find_path("a.b[2]") == nullptr);
    CHECK(v.find_path("a.zzz.c") == nullptr);
    CHECK(v.find_path("") == &v);
    CHECK_THROWS_AS(v.at_path("a.nope"), DataException);
    CHECK_THROWS_AS(v.find_path("a..b"), DataException);
    CHECK_THROWS_AS(v.find_path("a.b[x]"), DataException);
    CHECK_THROWS_AS(v.find_path("a.b[1"), DataException);
    CHECK_THROWS_AS(v.find_path("[\"abc"), DataException);
    v.ensure_path("x.y[1].z") = 5;
    CHECK_EQ(v.at_path("x.y[1].z").as_int(), i64{5});
    CHECK(v.at_path("x.y[0]").is_null());
}

FLYG_TEST(data_value_ordering) {
    CHECK(Value() < Value(false));
    CHECK(Value(true) < Value(0));
    CHECK(Value(1) < Value("a"));
    CHECK(Value("a") < Value(Bytes{}));
    CHECK(Value(Bytes{}) < Value(Array{}));
    CHECK(Value(Array{}) < Value(Object{}));
    CHECK(Value(1) == Value(1.0));
    CHECK(Value(2) > Value(1.5));
    CHECK(Value(1) < Value(1.5));
    CHECK(Value(-1) > Value(-1.5));
    CHECK(Value(9007199254740993LL) > Value(9007199254740992.0));  // exact int/double compare
    CHECK(Value(std::numeric_limits<i64>::max()) < Value(9223372036854775808.0));
    CHECK(Value(std::numeric_limits<double>::quiet_NaN()) == Value(std::numeric_limits<double>::quiet_NaN()));
    CHECK(Value(std::numeric_limits<double>::quiet_NaN()) < Value(-1e300));
    CHECK(Value(std::numeric_limits<double>::quiet_NaN()) < Value(i64{-5}));
    CHECK(Value("abc") < Value("abd"));
    CHECK(Value::array({1, 2}) < Value::array({1, 2, 0}));
    CHECK(Value::array({1, 3}) > Value::array({1, 2, 9}));
    // object equality ignores member order
    CHECK(Value::object({{"a", 1}, {"b", 2}}) == Value::object({{"b", 2}, {"a", 1}}));
    CHECK(Value::object({{"a", 1}}) != Value::object({{"a", 2}}));
    CHECK(Value::object({{"a", 1}}) < Value::object({{"b", 0}}));
    // transitivity spot-check across mixed types
    const std::vector<Value> vs = {Value(), Value(false), Value(true), Value(-2), Value(0.5), Value(1), Value(1.0), Value("a"),
                                   Value(Bytes{std::byte{1}}), Value::array({1}), Value::object({{"k", 1}})};
    for (const auto& a : vs)
        for (const auto& b : vs)
            for (const auto& c : vs)
                if (a <= b && b <= c) CHECK(a <= c);
}

// ---- JSON ------------------------------------------------------------------------------------
FLYG_TEST(data_json_parse_values) {
    CHECK(json::parse("null").is_null());
    CHECK(json::parse(" true ").as_bool());
    CHECK(!json::parse("false").as_bool());
    CHECK_EQ(json::parse("0").as_int(), i64{0});
    CHECK_EQ(json::parse("-0").as_int(), i64{0});
    CHECK_EQ(json::parse("123456789012").as_int(), i64{123456789012LL});
    CHECK_EQ(json::parse("-9223372036854775808").as_int(), std::numeric_limits<i64>::min());
    CHECK(json::parse("9223372036854775808").is_real());  // beyond i64 becomes a double
    CHECK_EQ(json::parse("1.5").type(), Type::real);
    CHECK_EQ(json::parse("1e2").as_double(), 100.0);
    CHECK_EQ(json::parse("1E-2").as_double(), 0.01);
    CHECK_EQ(json::parse("-1.25e+3").as_double(), -1250.0);
    CHECK_EQ(json::parse("0.1").as_double(), 0.1);
    CHECK(std::isinf(json::parse("1e999").as_double()));
    CHECK_EQ(json::parse("1e-999").as_double(), 0.0);
    CHECK_EQ(json::parse(R"("a\"b\\c\/d\b\f\n\r\t")").as_string(), "a\"b\\c/d\b\f\n\r\t");
    CHECK_EQ(json::parse(R"("\u0041\u00e9\ud55c\ud83d\ude00")").as_string(), "Aé한😀");
    CHECK_EQ(json::parse("\"한글 😀\"").as_string(), "한글 😀");
    CHECK_EQ(json::parse("\xEF\xBB\xBF[1]").size(), usize{1});  // BOM

    const Value v = json::parse(R"( { "a" : [1, 2.5, "x", null, true, {"n": []}], "b": {} } )");
    CHECK_EQ(v.at("a").size(), usize{6});
    CHECK(v.at("a").at(5).at("n").is_array());
    CHECK(v.at("b").is_object());
    CHECK(json::parse("[]").empty());
    CHECK(json::parse("{}").empty());
}

FLYG_TEST(data_json_parse_errors) {
    const char* bad[] = {"",        "   ",     "{",          "[1,",     "[1 2]",       "{\"a\"}",    "{\"a\":}",    "{a:1}",
                         "[01]",    "[1.]",    "[.5]",       "[1e]",    "[+1]",        "[--1]",      "nul",         "tru",
                         "\"abc",   "\"\\x\"", "\"\\u12\"",  "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"a\nb\"", "[1] x",
                         "[1,]",    "{\"a\":1,}", "{\"a\":1 \"b\":2}", "NaN", "Infinity", "// c\n1",    "/* c */ 1",   "'a'",
                         "\"\xFF\"", "[\"\xC0\xAF\"]", "[1,2", "{\"a\":[}"};
    for (const char* b : bad) {
        bool threw = false;
        try {
            (void)json::parse(b);
        } catch (const DataException&) {
            threw = true;
        }
        if (!threw) flyg_test::report_failure(__FILE__, __LINE__, std::string("should have failed: ") + b);
        ++flyg_test::check_count();
    }
    try {
        (void)json::parse("{\n  \"a\": 1,\n  \"b\": oops\n}");
        CHECK(false);
    } catch (const DataException& e) {
        CHECK(e.message().find("line 3, column 8") != std::string::npos);
        CHECK(e.offset() != DataException::no_offset);
    }
    Value out;
    CHECK(!json::try_parse("{", out));
    CHECK(json::try_parse("[1]", out));
    CHECK_EQ(out.size(), usize{1});
}

FLYG_TEST(data_json_parse_options) {
    json::ParseOptions o;
    o.allow_comments = true;
    CHECK_EQ(json::parse("// lead\n[1, /* mid */ 2] // tail", o).size(), usize{2});
    CHECK_THROWS_AS(json::parse("/* open", o), DataException);
    o = {};
    o.allow_trailing_commas = true;
    CHECK_EQ(json::parse("[1,2,]", o).size(), usize{2});
    CHECK_EQ(json::parse("{\"a\":1,}", o).size(), usize{1});
    o = {};
    o.allow_nan_inf = true;
    const Value n = json::parse("[NaN, Infinity, -Infinity]", o);
    CHECK(std::isnan(n.at(0).as_double()));
    CHECK(std::isinf(n.at(1).as_double()) && n.at(2).as_double() < 0);
    o = {};
    CHECK_EQ(json::parse(R"({"a":1,"a":2})").at("a").as_int(), i64{2});  // last wins
    o.reject_duplicate_keys = true;
    CHECK_THROWS_AS(json::parse(R"({"a":1,"a":2})", o), DataException);
    o = {};
    o.max_depth = 10;
    CHECK_NOTHROW(json::parse(std::string(9, '[') + std::string(9, ']'), o));
    CHECK_THROWS_AS(json::parse(std::string(11, '[') + std::string(11, ']'), o), DataException);
    // default limit protects against stack exhaustion
    CHECK_THROWS_AS(json::parse(std::string(100000, '[')), DataException);
}

FLYG_TEST(data_json_stringify) {
    CHECK_EQ(json::stringify(Value()), "null");
    CHECK_EQ(json::stringify(Value(true)), "true");
    CHECK_EQ(json::stringify(Value(-5)), "-5");
    CHECK_EQ(json::stringify(Value(1.5)), "1.5");
    CHECK_EQ(json::stringify(Value(3.0)), "3.0");  // stays a real
    CHECK_EQ(json::stringify(Value(1e21)), "1e+21");
    CHECK_EQ(json::stringify(Value(0.1)), "0.1");
    CHECK_EQ(json::stringify(Value("a\"b\\c\n\t\x01 é")), "\"a\\\"b\\\\c\\n\\t\\u0001 é\"");
    json::WriteOptions ascii;
    ascii.ascii_only = true;
    CHECK_EQ(json::stringify(Value("é😀"), ascii), "\"\\u00e9\\ud83d\\ude00\"");
    CHECK_EQ(json::stringify(Value::object({{"b", 1}, {"a", Value::array({1, 2})}})), R"({"b":1,"a":[1,2]})");
    json::WriteOptions sorted;
    sorted.sort_keys = true;
    CHECK_EQ(json::stringify(Value::object({{"b", 1}, {"a", 2}}), sorted), R"({"a":2,"b":1})");
    CHECK_EQ(json::pretty(Value::object({{"a", Value::array({1, 2})}, {"e", Value::object()}, {"f", Value::array()}}), 2),
             "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"e\": {},\n  \"f\": []\n}");
    CHECK_EQ(json::stringify(Value(Bytes{std::byte{'h'}, std::byte{'i'}})), "\"aGk=\"");
    json::WriteOptions nobin;
    nobin.binary_as_base64 = false;
    CHECK_THROWS_AS(json::stringify(Value(Bytes{}), nobin), DataException);
    CHECK_THROWS_AS(json::stringify(Value(std::nan(""))), DataException);
    CHECK_THROWS_AS(json::stringify(Value(HUGE_VAL)), DataException);
    CHECK_THROWS_AS(json::stringify(Value("\xFF")), DataException);
    json::WriteOptions ni;
    ni.allow_nan_inf = true;
    CHECK_EQ(json::stringify(Value::array({HUGE_VAL, -HUGE_VAL}), ni), "[Infinity,-Infinity]");
}

FLYG_TEST(data_json_round_trip) {
    // doubles must survive text round trips bit-exactly
    const double samples[] = {0.0, -0.0, 1.0, 0.1, 1.0 / 3.0, 3.141592653589793, 1e-300, 1.7976931348623157e308, 5e-324,
                              123456789.123456789, 2.2250738585072014e-308, 1e22, 4.35, 9007199254740993.0};
    for (double d : samples) {
        const double back = json::parse(json::stringify(Value(d))).as_double();
        CHECK(std::memcmp(&back, &d, sizeof d) == 0);
    }
    const i64 ints[] = {0, 1, -1, 123456789, std::numeric_limits<i64>::max(), std::numeric_limits<i64>::min()};
    for (i64 i : ints) CHECK_EQ(json::parse(json::stringify(Value(i))).as_int(), i);

    const std::string doc = R"({"s":"한글\n","n":[1,-2,3.5,null,true,false],"o":{"x":{"y":[[],{}]}}})";
    const Value v = json::parse(doc);
    CHECK(json::parse(json::stringify(v)) == v);
    CHECK(json::parse(json::pretty(v, 4)) == v);
    CHECK_EQ(json::stringify(v), doc.substr(0, 0) + R"({"s":"한글\n","n":[1,-2,3.5,null,true,false],"o":{"x":{"y":[[],{}]}}})");

    // Deterministic pseudo-random documents
    u64 seed = 12345;
    auto rnd = [&] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(seed >> 33); };
    std::function<Value(int)> gen = [&](int depth) -> Value {
        switch (rnd() % (depth > 3 ? 5 : 7)) {
            case 0: return Value();
            case 1: return Value(rnd() % 2 == 0);
            case 2: return Value(static_cast<i64>(rnd()) - 1000000000);
            case 3: return Value(static_cast<double>(rnd()) / 97.0);
            case 4: { std::string s; for (u32 i = rnd() % 8; i > 0; --i) { static const char* const pieces[] = {"a", "é", "\"", "\\", "\n", "한", "😀", " "}; s += pieces[rnd() % 8]; } return Value(s); }
            case 5: { Array a; for (u32 i = rnd() % 5; i > 0; --i) a.push_back(gen(depth + 1)); return Value(a); }
            default: { Object o; for (u32 i = rnd() % 5; i > 0; --i) o.insert_or_assign("k" + std::to_string(rnd() % 20), gen(depth + 1)); return Value(o); }
        }
    };
    for (int i = 0; i < 300; ++i) {
        const Value g = gen(0);
        CHECK(json::parse(json::stringify(g)) == g);
        CHECK(json::parse(json::pretty(g)) == g);
    }
}

FLYG_TEST(data_json_events_and_writer) {
    struct Rec : json::Handler {
        std::string log;
        void null_value() override { log += "N"; }
        void bool_value(bool b) override { log += b ? "T" : "F"; }
        void integer_value(i64 i) override { log += "i" + std::to_string(i); }
        void real_value(double) override { log += "r"; }
        void string_value(std::string_view s) override { log += "s(" + std::string(s) + ")"; }
        void start_object() override { log += "{"; }
        void key(std::string_view k) override { log += "k(" + std::string(k) + ")"; }
        void end_object() override { log += "}"; }
        void start_array() override { log += "["; }
        void end_array() override { log += "]"; }
    } rec;
    json::parse_events(R"({"a":[1,2.5,"x",null],"b":{"c":true}})", rec);
    CHECK_EQ(rec.log, "{k(a)[i1rs(x)N]k(b){k(c)T}}");

    json::Writer w;
    w.begin_object().key("a").begin_array().value(1).value("two").null().end_array().key("b").value(2.5).end_object();
    CHECK(w.complete());
    CHECK_EQ(w.take(), R"({"a":[1,"two",null],"b":2.5})");

    json::WriteOptions p;
    p.indent = 2;
    json::Writer pw(p);
    pw.begin_array().begin_object().key("k").value(true).end_object().begin_array().end_array().end_array();
    CHECK_EQ(pw.take(), "[\n  {\n    \"k\": true\n  },\n  []\n]");

    json::Writer bad;
    CHECK_THROWS_AS(bad.end_object(), DataException);
    CHECK_THROWS_AS(bad.key("x"), DataException);
    bad.begin_object();
    CHECK_THROWS_AS(bad.value(1), DataException);  // value without key
    bad.key("a");
    CHECK_THROWS_AS(bad.key("b"), DataException);  // two keys in a row
    bad.value(1);
    CHECK_THROWS_AS(bad.end_array(), DataException);
    bad.end_object();
    CHECK(bad.complete());
    CHECK_THROWS_AS(bad.value(2), DataException);  // second root

    std::ostringstream sink;
    json::Writer sw(sink);
    sw.begin_array();
    for (int i = 0; i < 50000; ++i) sw.value(i);
    sw.end_array();
    sw.flush();
    const Value back = json::parse(sink.str());
    CHECK_EQ(back.size(), usize{50000});
    CHECK_EQ(back.at(49999).as_int(), i64{49999});
}

FLYG_TEST(data_json_ndjson_pointer_patch) {
    std::istringstream in("{\"a\":1}\n\n{\"a\":2}\r\n[3]\n");
    std::vector<Value> got;
    const usize n = json::read_ndjson(in, [&](Value&& v) { got.push_back(std::move(v)); return true; });
    CHECK_EQ(n, usize{3});
    CHECK_EQ(got[1].at("a").as_int(), i64{2});
    CHECK(got[2].is_array());
    std::istringstream stop("1\n2\n3\n");
    CHECK_EQ(json::read_ndjson(stop, [&](Value&&) { return false; }), usize{1});
    std::istringstream bad("1\n{oops\n");
    try {
        json::read_ndjson(bad, [](Value&&) { return true; });
        CHECK(false);
    } catch (const DataException& e) {
        CHECK(e.message().find("NDJSON line 2") != std::string::npos);
    }
    std::ostringstream out;
    json::write_ndjson_line(out, Value::object({{"x", 1}}));
    json::write_ndjson_line(out, Value::array({1, 2}));
    CHECK_EQ(out.str(), "{\"x\":1}\n[1,2]\n");

    const Value doc = json::parse(R"({"a":{"b":[10,20]},"x/y":1,"m~n":2,"":3})");
    CHECK_EQ(json::pointer(doc, "/a/b/1")->as_int(), i64{20});
    CHECK_EQ(json::pointer(doc, "/x~1y")->as_int(), i64{1});
    CHECK_EQ(json::pointer(doc, "/m~0n")->as_int(), i64{2});
    CHECK_EQ(json::pointer(doc, "/")->as_int(), i64{3});
    CHECK(json::pointer(doc, "") == &doc);
    CHECK(json::pointer(doc, "/a/b/2") == nullptr);
    CHECK(json::pointer(doc, "/a/b/01") == nullptr);
    CHECK(json::pointer(doc, "/a/zz") == nullptr);
    CHECK_THROWS_AS(json::pointer(doc, "a"), DataException);
    CHECK_THROWS_AS(json::pointer(doc, "/a~2"), DataException);

    Value target = json::parse(R"({"a":"b","c":{"d":"e","f":"g"},"h":[1]})");
    json::merge_patch(target, json::parse(R"({"a":"z","c":{"f":null},"h":[2],"n":{"x":1}})"));
    CHECK_EQ(json::stringify(target), R"({"a":"z","c":{"d":"e"},"h":[2],"n":{"x":1}})");
    Value scalar = 5;
    json::merge_patch(scalar, json::parse(R"({"k":1})"));
    CHECK_EQ(json::stringify(scalar), R"({"k":1})");
}
