// flyG - data module tests: TOML
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>

#include <cmath>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

namespace {
std::string S(const Value& v) {
    json::WriteOptions o;
    o.sort_keys = true;
    return json::stringify(v, o);
}
Value T(std::string_view s) { return toml::parse(s); }
}  // namespace

FLYG_TEST(data_toml_basic) {
    const Value v = T(
        "# comment\n"
        "title = \"TOML Example\"   # trailing\n"
        "enabled = true\n"
        "off = false\n"
        "count = 42\n"
        "neg = -17\n"
        "plus = +5\n"
        "pi = 3.14159\n"
        "exp = 5e+22\n"
        "mixed = -1.5E-3\n"
        "big = 9_223_372_036_854_775_807\n"
        "mini = -9223372036854775808\n"
        "under = 1_000\n"
        "hex = 0xDEAD_beef\n"
        "oct = 0o755\n"
        "bin = 0b1101\n"
        "zero = 0\n"
        "inf1 = inf\n"
        "inf2 = -inf\n"
        "nan1 = nan\n"
        "empty = \"\"\n"
        "\"quoted key\" = 1\n"
        "'literal key' = 2\n"
        "bare-key_1 = 3\n"
        "1234 = 4\n");
    CHECK_EQ(v.at("title").as_string(), "TOML Example");
    CHECK(v.at("enabled").as_bool() && !v.at("off").as_bool());
    CHECK_EQ(v.at("count").as_int(), i64{42});
    CHECK_EQ(v.at("neg").as_int(), i64{-17});
    CHECK_EQ(v.at("plus").as_int(), i64{5});
    CHECK_EQ(v.at("pi").as_double(), 3.14159);
    CHECK_EQ(v.at("exp").as_double(), 5e22);
    CHECK_EQ(v.at("mixed").as_double(), -1.5e-3);
    CHECK_EQ(v.at("big").as_int(), std::numeric_limits<i64>::max());
    CHECK_EQ(v.at("mini").as_int(), std::numeric_limits<i64>::min());
    CHECK_EQ(v.at("under").as_int(), i64{1000});
    CHECK_EQ(v.at("hex").as_int(), i64{0xDEADBEEF});
    CHECK_EQ(v.at("oct").as_int(), i64{0755});
    CHECK_EQ(v.at("bin").as_int(), i64{13});
    CHECK(v.at("zero").is_integer());
    CHECK(std::isinf(v.at("inf1").as_double()) && v.at("inf2").as_double() < 0);
    CHECK(std::isnan(v.at("nan1").as_double()));
    CHECK_EQ(v.at("empty").as_string(), "");
    CHECK_EQ(v.at("quoted key").as_int(), i64{1});
    CHECK_EQ(v.at("literal key").as_int(), i64{2});
    CHECK_EQ(v.at("bare-key_1").as_int(), i64{3});
    CHECK_EQ(v.at("1234").as_int(), i64{4});
    CHECK(T("").is_object() && T("").empty());
    CHECK(T("# nothing\n\n  \n").empty());
    CHECK_EQ(S(T("a=1\r\nb=2\r\n")), R"({"a":1,"b":2})");
}

FLYG_TEST(data_toml_strings) {
    const Value v = T(
        "basic = \"tab\\there \\\"q\\\" \\\\ \\u00e9 \\U0001F600 \\n\"\n"
        "literal = 'C:\\path\\no\\escapes'\n"
        "ml = \"\"\"\n"
        "Roses are red\n"
        "Violets are blue\"\"\"\n"
        "ml_trim = \"\"\"\\\n"
        "    The quick brown \\\n"
        "    fox jumps\\\n"
        "    \"\"\"\n"
        "ml_quotes = \"\"\"say \"hi\" and \"\"bye\"\"\"\"\"\n"
        "mll = '''\n"
        "raw \\n text\n"
        "  keeps indent\n"
        "'''\n"
        "mll_q = '''it''s'''\n"
        "unicode = \"한글 😀\"\n");
    CHECK_EQ(v.at("basic").as_string(), "tab\there \"q\" \\ é 😀 \n");
    CHECK_EQ(v.at("literal").as_string(), "C:\\path\\no\\escapes");
    CHECK_EQ(v.at("ml").as_string(), "Roses are red\nViolets are blue");
    CHECK_EQ(v.at("ml_trim").as_string(), "The quick brown fox jumps");
    CHECK_EQ(v.at("ml_quotes").as_string(), "say \"hi\" and \"\"bye\"\"");
    CHECK_EQ(v.at("mll").as_string(), "raw \\n text\n  keeps indent\n");
    CHECK_EQ(v.at("mll_q").as_string(), "it''s");
    CHECK_EQ(v.at("unicode").as_string(), "한글 😀");
    CHECK_EQ(T("a = \"\"\"\r\nx\r\ny\"\"\"").at("a").as_string(), "x\ny");
}

FLYG_TEST(data_toml_datetimes) {
    const Value v = T(
        "odt1 = 1979-05-27T07:32:00Z\n"
        "odt2 = 1979-05-27T00:32:00.999999-07:00\n"
        "odt3 = 1979-05-27 07:32:00Z\n"
        "ldt = 1979-05-27T07:32:00\n"
        "ld = 1979-05-27\n"
        "lt = 07:32:00\n"
        "ltf = 00:32:00.5\n"
        "lower = 1979-05-27t07:32:00z\n"
        "arr = [1979-05-27, 07:32:00]\n");
    CHECK_EQ(v.at("odt1").as_string(), "1979-05-27T07:32:00Z");
    CHECK_EQ(v.at("odt2").as_string(), "1979-05-27T00:32:00.999999-07:00");
    CHECK_EQ(v.at("odt3").as_string(), "1979-05-27 07:32:00Z");
    CHECK_EQ(v.at("ldt").as_string(), "1979-05-27T07:32:00");
    CHECK_EQ(v.at("ld").as_string(), "1979-05-27");
    CHECK_EQ(v.at("lt").as_string(), "07:32:00");
    CHECK_EQ(v.at("ltf").as_string(), "00:32:00.5");
    CHECK_EQ(v.at("lower").as_string(), "1979-05-27T07:32:00Z");
    CHECK_EQ(S(v.at("arr")), R"(["1979-05-27","07:32:00"])");
    CHECK_THROWS_AS(T("d = 2023-02-29"), DataException);
    CHECK_THROWS_AS(T("d = 2024-13-01"), DataException);
    CHECK_THROWS_AS(T("d = 2024-01-32"), DataException);
    CHECK_THROWS_AS(T("t = 25:00:00"), DataException);
    CHECK_THROWS_AS(T("t = 12:61:00"), DataException);
    CHECK_NOTHROW(T("d = 2024-02-29"));
}

FLYG_TEST(data_toml_tables) {
    const Value v = T(
        "top = 1\n"
        "[owner]\n"
        "name = \"Tom\"\n"
        "[database]\n"
        "ports = [8000, 8001, 8002]\n"
        "temp_targets = { cpu = 79.5, case = 72.0 }\n"
        "[servers.alpha]\n"
        "ip = \"10.0.0.1\"\n"
        "[servers.beta]\n"
        "ip = \"10.0.0.2\"\n"
        "[a.b.c]\n"
        "deep = true\n"
        "[a]\n"            // defining a parent after its child is allowed
        "late = 1\n"
        "[ spaced . header ]\n"
        "x = 1\n"
        "[\"quoted.header\".\"y z\"]\n"
        "k = 2\n");
    CHECK_EQ(S(v),
             R"({"a":{"b":{"c":{"deep":true}},"late":1},"database":{"ports":[8000,8001,8002],"temp_targets":{"case":72.0,"cpu":79.5}},)"
             R"("owner":{"name":"Tom"},"quoted.header":{"y z":{"k":2}},"servers":{"alpha":{"ip":"10.0.0.1"},"beta":{"ip":"10.0.0.2"}},)"
             R"("spaced":{"header":{"x":1}},"top":1})");

    // dotted keys
    const Value d = T("a.b.c = 1\na.b.d = 2\na . e = 3\n[t]\nx.y = 4\n");
    CHECK_EQ(S(d), R"({"a":{"b":{"c":1,"d":2},"e":3},"t":{"x":{"y":4}}})");
    // sub-table of a dotted table is fine, the dotted table itself cannot be re-opened
    CHECK_NOTHROW(T("[fruit]\napple.color = \"red\"\n[fruit.apple.texture]\nsmooth = true\n"));
    CHECK_THROWS_AS(T("[fruit]\napple.color = \"red\"\n[fruit.apple]\nx = 1\n"), DataException);
}

FLYG_TEST(data_toml_arrays_and_inline) {
    const Value v = T(
        "ints = [1, 2, 3]\n"
        "mixed = [1, \"two\", 3.0, [4, 5], { six = 6 }]\n"
        "multi = [\n"
        "  1, # one\n"
        "  2,\n"
        "  3,   # trailing comma ok\n"
        "]\n"
        "empty = []\n"
        "nested = [[1, 2], [\"a\"]]\n"
        "pt = { x = 1, y = 2 }\n"
        "dotted_inline = { a.b = 1, a.c = 2 }\n"
        "nested_inline = { inner = { deep = [1, { z = 0 }] } }\n"
        "empty_inline = {}\n");
    CHECK_EQ(S(v.at("ints")), "[1,2,3]");
    CHECK_EQ(S(v.at("mixed")), R"([1,"two",3.0,[4,5],{"six":6}])");
    CHECK_EQ(S(v.at("multi")), "[1,2,3]");
    CHECK_EQ(S(v.at("empty")), "[]");
    CHECK_EQ(S(v.at("nested")), R"([[1,2],["a"]])");
    CHECK_EQ(S(v.at("pt")), R"({"x":1,"y":2})");
    CHECK_EQ(S(v.at("dotted_inline")), R"({"a":{"b":1,"c":2}})");
    CHECK_EQ(S(v.at("nested_inline")), R"({"inner":{"deep":[1,{"z":0}]}})");
    CHECK_EQ(S(v.at("empty_inline")), "{}");
}

FLYG_TEST(data_toml_array_of_tables) {
    const Value v = T(
        "[[products]]\n"
        "name = \"Hammer\"\n"
        "sku = 738594937\n"
        "[[products]]\n"
        "[[products]]\n"
        "name = \"Nail\"\n"
        "[products.dims]\n"          // sub-table of the last element
        "w = 1\n"
        "[[fruits]]\n"
        "name = \"apple\"\n"
        "[fruits.physical]\n"
        "color = \"red\"\n"
        "[[fruits.varieties]]\n"
        "name = \"red delicious\"\n"
        "[[fruits.varieties]]\n"
        "name = \"granny smith\"\n"
        "[[fruits]]\n"
        "name = \"banana\"\n"
        "[[fruits.varieties]]\n"
        "name = \"plantain\"\n");
    CHECK_EQ(S(v.at("products")), R"([{"name":"Hammer","sku":738594937},{},{"dims":{"w":1},"name":"Nail"}])");
    CHECK_EQ(S(v.at("fruits")),
             R"([{"name":"apple","physical":{"color":"red"},"varieties":[{"name":"red delicious"},{"name":"granny smith"}]},)"
             R"({"name":"banana","varieties":[{"name":"plantain"}]}])");
}

FLYG_TEST(data_toml_errors) {
    const char* bad[] = {
        "a = 1\na = 2\n",                         // duplicate key
        "a = 1\n[a]\n",                           // table over value
        "[a]\n[a]\n",                             // table defined twice
        "[a]\nb = 1\n[a.b]\n",                    // table over value (nested)
        "a = {x = 1}\n[a]\n",                     // inline table is closed
        "a = {x = 1}\n[a.y]\n",
        "a = {x = 1}\na.y = 2\n",
        "[[a]]\n[a]\n",                           // AoT vs table
        "a = [1]\n[[a]]\n",                       // static array vs AoT
        "[a]\n[[a]]\n",
        "= 1\n",                                  // missing key
        "a =\n",                                  // missing value
        "a = \n",
        "a 1\n",
        "a = 1 b = 2\n",                          // two statements on a line
        "a = [1, 2\n",
        "a = {x = 1,}\n",
        "a = {x = 1\n}\n",
        "a = 01\n",                               // leading zero
        "a = 1__0\n",
        "a = _1\n",
        "a = 1_\n",
        "a = +0x10\n",
        "a = 0x\n",
        "a = 1.\n",
        "a = .5\n",
        "a = 1e\n",
        "a = inf1\n",
        "a = tru\n",
        "a = 9223372036854775808\n",              // out of range
        "a = \"open\n",
        "a = 'open\n",
        "a = \"\"\"open\n",
        "a = \"bad \\q\"\n",
        "a = \"\\ud800\"\n",
        "a = \"\\u12\"\n",
        "a = \"ctl\x01\"\n",
        "[a\nb = 1\n",
        "[]\n",
        "[a.]\n",
        "[[a]\n",
        "a.b = 1\na = 2\n",
        "a = 1\na.b = 2\n",
        "[x]\ny.z = 1\n[x.y]\n",
        "\xFF = 1\n",
        "a = 'x' # ok\nb = \"\n",
        "a = [1,,2]\n",
        "a = {,}\n",
    };
    for (const char* b : bad) {
        bool threw = false;
        try {
            (void)toml::parse(b);
        } catch (const DataException&) {
            threw = true;
        }
        if (!threw) flyg_test::report_failure(__FILE__, __LINE__, std::string("should have failed: ") + b);
        ++flyg_test::check_count();
    }
    try {
        (void)toml::parse("a = 1\nb = 2\nb = 3\n");
        CHECK(false);
    } catch (const DataException& e) {
        CHECK(e.message().find("duplicate key 'b'") != std::string::npos);
        CHECK(e.message().find("line 3") != std::string::npos);
    }
    CHECK_THROWS_AS(toml::parse(std::string("a = ") + std::string(1000, '[')), DataException);  // depth limit
}

FLYG_TEST(data_toml_write) {
    const Value doc = json::parse(R"({
        "title": "T", "n": 1, "f": 2.0, "b": true, "list": [1, 2], "mix": [1, "a", [2]],
        "a b": "spaced key", "empty": "", "esc": "q\"\\\n\t\u0001é",
        "server": {"host": "h", "ports": [1, 2], "tls": {"on": true}},
        "empty_tbl": {},
        "items": [{"id": 1, "tags": ["x"]}, {"id": 2, "sub": {"k": "v"}}],
        "objs_in_arr": [[{"a": 1}], 5],
        "inf": 1e999, "ninf": -1e999
    })", json::ParseOptions{});
    const std::string text = toml::write(doc);
    const Value back = toml::parse(text);
    CHECK(back == doc);
    CHECK(text.find("[server]") != std::string::npos);
    CHECK(text.find("[server.tls]") != std::string::npos);
    CHECK(text.find("[[items]]") != std::string::npos);
    CHECK(text.find("\"a b\" = ") != std::string::npos);
    CHECK(text.find("f = 2.0") != std::string::npos);

    // deterministic exact output for a small document
    CHECK_EQ(toml::write(json::parse(R"({"a":1,"t":{"b":"x"},"l":[{"c":true}]})")), "a = 1\n\n[t]\nb = \"x\"\n\n[[l]]\nc = true\n");

    CHECK_THROWS_AS(toml::write(Value::array({1})), DataException);
    CHECK_THROWS_AS(toml::write(json::parse(R"({"a":null})")), DataException);
    CHECK_THROWS_AS(toml::write(json::parse(R"({"a":[null]})")), DataException);
    CHECK_THROWS_AS(toml::write(Value::object({{"b", Value(Bytes{})}})), DataException);
    CHECK_THROWS_AS(toml::write(Value::object({{"s", Value("\xFF")}})), DataException);
    toml::WriteOptions skip;
    skip.skip_null = true;
    CHECK_EQ(toml::write(json::parse(R"({"a":null,"b":1})"), skip), "b = 1\n");
    // NaN survives
    const Value nanv = toml::parse(toml::write(Value::object({{"x", std::nan("")}})));
    CHECK(std::isnan(nanv.at("x").as_double()));
}

FLYG_TEST(data_toml_round_trip_random) {
    u64 seed = 4242;
    auto rnd = [&] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(seed >> 33); };
    static const char* const words[] = {"a", "key with space", "", "dot.ted", "q\"uote", "é한😀", "[x]", "1234", "-", "_", "a=b", "#h", "new\nline", "tab\t"};
    std::function<Value(int)> gen = [&](int depth) -> Value {
        switch (rnd() % (depth > 3 ? 5 : 8)) {
            case 0: return Value(rnd() % 2 == 0);
            case 1: return Value(static_cast<i64>(rnd()) - 1000000000);
            case 2: return Value(static_cast<double>(rnd()) / 7.0);
            case 3: case 4: return Value(std::string(words[rnd() % (sizeof words / sizeof words[0])]));
            case 5: { Array a; for (u32 i = rnd() % 4; i > 0; --i) a.push_back(gen(depth + 1)); return Value(a); }
            default: { Object o; for (u32 i = rnd() % 4; i > 0; --i) o.insert_or_assign(words[rnd() % (sizeof words / sizeof words[0])], gen(depth + 1)); return Value(o); }
        }
    };
    for (int i = 0; i < 500; ++i) {
        Object root;
        for (u32 k = 1 + rnd() % 4; k > 0; --k) root.insert_or_assign(words[rnd() % (sizeof words / sizeof words[0])], gen(0));
        const Value g(std::move(root));
        std::string text;
        try {
            text = toml::write(g);
            const Value back = toml::parse(text);
            ++flyg_test::check_count();
            if (!(back == g)) flyg_test::report_failure(__FILE__, __LINE__, "mismatch\n--- TOML:\n" + text + "--- got " + S(back) + "\n--- want " + S(g));
        } catch (const DataException& e) {
            flyg_test::report_failure(__FILE__, __LINE__, std::string(e.what()) + "\n--- TOML:\n" + text);
        }
    }
}
