// flyG - data module tests: YAML
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>

#include <cmath>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

namespace {
Value J(std::string_view s) { return json::parse(s); }
std::string S(const Value& v) { return json::stringify(v); }
}  // namespace

FLYG_TEST(data_yaml_scalars) {
    CHECK(yaml::parse("null").is_null());
    CHECK(yaml::parse("~").is_null());
    CHECK(yaml::parse("").is_null());
    CHECK(yaml::parse("# only a comment\n").is_null());
    CHECK(yaml::parse("true").as_bool());
    CHECK(!yaml::parse("False").as_bool());
    CHECK_EQ(yaml::parse("42").as_int(), i64{42});
    CHECK_EQ(yaml::parse("-17").as_int(), i64{-17});
    CHECK_EQ(yaml::parse("+5").as_int(), i64{5});
    CHECK_EQ(yaml::parse("0x1F").as_int(), i64{31});
    CHECK_EQ(yaml::parse("0o17").as_int(), i64{15});
    CHECK_EQ(yaml::parse("3.25").as_double(), 3.25);
    CHECK_EQ(yaml::parse("-1e3").as_double(), -1000.0);
    CHECK_EQ(yaml::parse(".5").as_double(), 0.5);
    CHECK_EQ(yaml::parse("5.").as_double(), 5.0);
    CHECK(std::isinf(yaml::parse(".inf").as_double()));
    CHECK(yaml::parse("-.inf").as_double() < 0);
    CHECK(std::isnan(yaml::parse(".NaN").as_double()));
    CHECK(yaml::parse("99999999999999999999").is_real());
    // things that look numeric but are strings
    CHECK_EQ(yaml::parse("1.2.3").as_string(), "1.2.3");
    CHECK_EQ(yaml::parse("0x").as_string(), "0x");
    CHECK_EQ(yaml::parse("1e").as_string(), "1e");
    CHECK_EQ(yaml::parse("yes").as_string(), "yes");  // YAML 1.2: not a bool
    CHECK_EQ(yaml::parse("hello world").as_string(), "hello world");
    CHECK_EQ(yaml::parse("한글 😀").as_string(), "한글 😀");
    CHECK_EQ(yaml::parse("a # comment").as_string(), "a");
    CHECK_EQ(yaml::parse("a#notcomment").as_string(), "a#notcomment");
    CHECK_EQ(yaml::parse("http://x.y/z").as_string(), "http://x.y/z");
    CHECK_EQ(yaml::parse("-x").as_string(), "-x");
}

FLYG_TEST(data_yaml_quoted) {
    CHECK_EQ(yaml::parse("'it''s'").as_string(), "it's");
    CHECK_EQ(yaml::parse("'a\\nb'").as_string(), "a\\nb");
    CHECK_EQ(yaml::parse(R"("a\tb\n\"q\" \\ \/ \x41\u00e9\U0001F600 \0")").as_string(), std::string("a\tb\n\"q\" \\ / Aé😀 \0", 21));
    CHECK_EQ(yaml::parse("\"\\ud83d\\ude00\"").as_string(), "😀");
    CHECK_EQ(yaml::parse("\"folded\n   line\"").as_string(), "folded line");
    CHECK_EQ(yaml::parse("\"para\n\n   two\"").as_string(), "para\ntwo");
    CHECK_EQ(yaml::parse("\"join\\\n   ed\"").as_string(), "joined");
    CHECK_EQ(yaml::parse("'x\n  y'").as_string(), "x y");
    CHECK_EQ(yaml::parse("\"123\"").as_string(), "123");
    CHECK_EQ(yaml::parse("'true'").as_string(), "true");
    CHECK_EQ(yaml::parse("''").as_string(), "");
    CHECK_THROWS_AS(yaml::parse("\"open"), DataException);
    CHECK_THROWS_AS(yaml::parse("'open"), DataException);
    CHECK_THROWS_AS(yaml::parse("\"bad \\q\""), DataException);
    CHECK_THROWS_AS(yaml::parse("\"\\ud800\""), DataException);
    CHECK_THROWS_AS(yaml::parse("\"\\xZZ\""), DataException);
}

FLYG_TEST(data_yaml_block_collections) {
    const Value v = yaml::parse(
        "# config\n"
        "name: flyG   # trailing comment\n"
        "version: 0.1\n"
        "enabled: true\n"
        "nothing:\n"
        "empty_str: ''\n"
        "tags:\n"
        "  - a\n"
        "  - b\n"
        "  - 3\n"
        "nested:\n"
        "  deep:\n"
        "    deeper: [1, 2]\n"
        "  other: x\n"
        "compact_seq:\n"
        "- p\n"
        "- q\n"
        "list_of_maps:\n"
        "  - id: 1\n"
        "    name: one\n"
        "  - id: 2\n"
        "    name: two\n"
        "    extra:\n"
        "      - x\n"
        "  -\n"
        "    id: 3\n"
        "matrix:\n"
        "  - - 1\n"
        "    - 2\n"
        "  - - 3\n");
    CHECK_EQ(v.at("name").as_string(), "flyG");
    CHECK_EQ(v.at("version").as_double(), 0.1);
    CHECK(v.at("enabled").as_bool());
    CHECK(v.at("nothing").is_null());
    CHECK_EQ(v.at("empty_str").as_string(), "");
    CHECK_EQ(S(v.at("tags")), R"(["a","b",3])");
    CHECK_EQ(S(v.at("nested")), R"({"deep":{"deeper":[1,2]},"other":"x"})");
    CHECK_EQ(S(v.at("compact_seq")), R"(["p","q"])");
    CHECK_EQ(S(v.at("list_of_maps")), R"([{"id":1,"name":"one"},{"id":2,"name":"two","extra":["x"]},{"id":3}])");
    CHECK_EQ(S(v.at("matrix")), "[[1,2],[3]]");

    CHECK_EQ(S(yaml::parse("- a\n- b\n")), R"(["a","b"])");
    CHECK_EQ(S(yaml::parse("-\n- x\n-\n")), R"([null,"x",null])");
    CHECK_EQ(S(yaml::parse("123: int key\ntrue: bool key\nnull: n\n1.5: f\n")), R"({"123":"int key","true":"bool key","null":"n","1.5":"f"})");
    CHECK_EQ(S(yaml::parse("\"quoted key\": 1\n'single': 2\n")), R"({"quoted key":1,"single":2})");
    CHECK_EQ(S(yaml::parse("a:\n  - 1\n  - 2\nb: 3\n")), R"({"a":[1,2],"b":3})");
    CHECK_EQ(S(yaml::parse("key:   spaced value  \n")), R"({"key":"spaced value"})");
    CHECK_EQ(S(yaml::parse("url: http://a.b:80/c\ntime: 12:30:45\n")), R"({"url":"http://a.b:80/c","time":"12:30:45"})");
}

FLYG_TEST(data_yaml_multiline_plain) {
    CHECK_EQ(S(yaml::parse("text: this is\n  a long\n  plain scalar\nnext: 1\n")), R"({"text":"this is a long plain scalar","next":1})");
    CHECK_EQ(S(yaml::parse("text: para one\n\n  para two\n")), R"({"text":"para one\npara two"})");
    CHECK_EQ(S(yaml::parse("- first\n  line\n- second\n")), R"(["first line","second"])");
    CHECK_THROWS_AS(yaml::parse("a: b # c\n  d\n"), DataException);  // a comment ends a plain scalar
}

FLYG_TEST(data_yaml_block_scalars) {
    const Value v = yaml::parse(
        "lit: |\n"
        "  line one\n"
        "   indented\n"
        "\n"
        "  line three\n"
        "fold: >\n"
        "  folded\n"
        "  into one\n"
        "\n"
        "  new para\n"
        "   more indented\n"
        "  back\n"
        "strip: |-\n"
        "  no trailing\n"
        "\n"
        "keep: |+\n"
        "  kept\n"
        "\n"
        "\n"
        "clip: |\n"
        "  clipped\n"
        "\n"
        "explicit: |2\n"
        "    two extra spaces\n"
        "  base\n"
        "last: end\n");
    CHECK_EQ(v.at("lit").as_string(), "line one\n indented\n\nline three\n");
    CHECK_EQ(v.at("fold").as_string(), "folded into one\nnew para\n more indented\nback\n");
    CHECK_EQ(v.at("strip").as_string(), "no trailing");
    CHECK_EQ(v.at("keep").as_string(), "kept\n\n\n");
    CHECK_EQ(v.at("clip").as_string(), "clipped\n");
    CHECK_EQ(v.at("explicit").as_string(), "  two extra spaces\nbase\n");
    CHECK_EQ(v.at("last").as_string(), "end");
    CHECK_EQ(S(yaml::parse("- |\n  a\n  b\n- >-\n  c\n  d\n")), R"(["a\nb\n","c d"])");
    CHECK_EQ(yaml::parse("|\n  top level\n  block\n").as_string(), "top level\nblock\n");
    CHECK_EQ(yaml::parse("k: |\n").at("k").as_string(), "");
    CHECK_THROWS_AS(yaml::parse("k: | junk\n  x\n"), DataException);
}

FLYG_TEST(data_yaml_flow) {
    CHECK_EQ(S(yaml::parse("[1, two, 3.5, true, null, ~]")), R"([1,"two",3.5,true,null,null])");
    CHECK_EQ(S(yaml::parse("{a: 1, b: [x, y], c: {d: e}, f}")), R"({"a":1,"b":["x","y"],"c":{"d":"e"},"f":null})");
    CHECK_EQ(S(yaml::parse("{\"a\":1,\"b\":\"two\"}")), R"({"a":1,"b":"two"})");  // JSON is YAML
    CHECK_EQ(S(yaml::parse("k: [a, b,\n  c]\nj: {x: 1,\n  y: 2}\n")), R"({"k":["a","b","c"],"j":{"x":1,"y":2}})");
    CHECK_EQ(S(yaml::parse("[]")), "[]");
    CHECK_EQ(S(yaml::parse("{}")), "{}");
    CHECK_EQ(S(yaml::parse("[a, [b, [c]], {d: [e]}]")), R"(["a",["b",["c"]],{"d":["e"]}])");
    CHECK_EQ(S(yaml::parse("[a: 1, b: 2]")), R"([{"a":1},{"b":2}])");
    CHECK_EQ(S(yaml::parse("[ 'q, r', \"s]\", t u ]")), R"(["q, r","s]","t u"])");
    CHECK_EQ(S(yaml::parse("{a: 1, # comment\n b: 2}")), R"({"a":1,"b":2})");
    CHECK_EQ(S(yaml::parse("[1,2,]")), "[1,2]");
    CHECK_THROWS_AS(yaml::parse("[1, 2"), DataException);
    CHECK_THROWS_AS(yaml::parse("{a: 1"), DataException);
    CHECK_THROWS_AS(yaml::parse("[1 2 }"), DataException);
    CHECK_THROWS_AS(yaml::parse("{a: 1, a: 2}"), DataException);
}

FLYG_TEST(data_yaml_anchors_aliases_tags) {
    const Value v = yaml::parse(
        "base: &base\n"
        "  host: localhost\n"
        "  port: 80\n"
        "dev:\n"
        "  <<: *base\n"
        "  port: 8080\n"
        "multi:\n"
        "  <<: [*base, {extra: 1}]\n"
        "scalar: &s hello\n"
        "again: *s\n"
        "list: &l [1, 2]\n"
        "copy: *l\n"
        "inline_alias: [*s, *s]\n");
    CHECK_EQ(S(v.at("dev")), R"({"host":"localhost","port":8080})");
    CHECK_EQ(S(v.at("multi")), R"({"host":"localhost","port":80,"extra":1})");
    CHECK_EQ(v.at("again").as_string(), "hello");
    CHECK_EQ(S(v.at("copy")), "[1,2]");
    CHECK_EQ(S(v.at("inline_alias")), R"(["hello","hello"])");
    CHECK_THROWS_AS(yaml::parse("a: *nope"), DataException);

    // billion laughs is stopped by the expansion limit
    std::string bomb = "a: &a [x, x, x, x, x, x, x, x, x, x]\n";
    char prev = 'a';
    for (char c = 'b'; c <= 'j'; ++c) {
        bomb += std::string(1, c) + ": &" + c + " [";
        for (int i = 0; i < 10; ++i) bomb += std::string(i ? ", *" : "*") + prev;
        bomb += "]\n";
        prev = c;
    }
    CHECK_THROWS_AS(yaml::parse(bomb), DataException);

    CHECK_EQ(yaml::parse("!!str 123").as_string(), "123");
    CHECK_EQ(yaml::parse("!!int '42'").as_int(), i64{42});
    CHECK_EQ(yaml::parse("!!float 3").as_double(), 3.0);
    CHECK(yaml::parse("!!float 3").is_real());
    CHECK(yaml::parse("!!null anything").is_null());
    CHECK_EQ(yaml::parse("k: !!str true").at("k").as_string(), "true");
    CHECK(yaml::parse("!!binary aGk=").as_binary() == (Bytes{std::byte{'h'}, std::byte{'i'}}));
    CHECK_EQ(yaml::parse("!custom value").as_string(), "value");  // unknown tag ignored
    CHECK_THROWS_AS(yaml::parse("!!int abc"), DataException);
    CHECK_THROWS_AS(yaml::parse("!!bool maybe"), DataException);
}

FLYG_TEST(data_yaml_documents) {
    const auto docs = yaml::parse_all("---\na: 1\n---\n- x\n- y\n...\n---\nplain\n");
    CHECK_EQ(docs.size(), usize{3});
    CHECK_EQ(S(docs[0]), R"({"a":1})");
    CHECK_EQ(S(docs[1]), R"(["x","y"])");
    CHECK_EQ(docs[2].as_string(), "plain");
    CHECK_THROWS_AS(yaml::parse("a: 1\n---\nb: 2\n"), DataException);  // single-document API
    CHECK_EQ(S(yaml::parse("%YAML 1.2\n---\na: 1\n")), R"({"a":1})");
    CHECK_EQ(S(yaml::parse("--- \na: 1\n")), R"({"a":1})");
    CHECK_EQ(S(yaml::parse("--- [1, 2]\n")), "[1,2]");
    CHECK(yaml::parse("---\n").is_null());
    CHECK_EQ(yaml::parse_all("a: 1\n---\n---\nb: 2\n").size(), usize{3});
    CHECK_EQ(yaml::parse("--- text\n").as_string(), "text");
}

FLYG_TEST(data_yaml_errors) {
    const char* bad[] = {"a: 1\n b: 2\n",           // over-indented
                         "a:\n  b: 1\n c: 2\n",     // under-indented
                         "a: 1\na: 2\n",            // duplicate key
                         "- a\nb: 1\n",             // mixing seq and map at one level
                         "a: b: c\n",               // nested mapping on one line
                         "a: - b\n",                // seq on the key line
                         "\ta: 1\n",                // tab indentation
                         "? complex\n: key\n",      // unsupported
                         "a: [1, 2\nb: 3\n",        // unterminated flow
                         "key: value\n  - x\n",
                         "a: @bad\n",
                         "a: 1\n- b\n",
                         "x: *\n",
                         "k: !!int [1]\n",
                         "{a: 1}: 2 3\n"};
    for (const char* b : bad) {
        bool threw = false;
        try {
            (void)yaml::parse(b);
        } catch (const DataException&) {
            threw = true;
        }
        if (!threw) flyg_test::report_failure(__FILE__, __LINE__, std::string("should have failed: ") + b);
        ++flyg_test::check_count();
    }
    try {
        (void)yaml::parse("a: 1\nb:\n  c: 2\n  c: 3\n");
        CHECK(false);
    } catch (const DataException& e) {
        CHECK(e.message().find("duplicate mapping key 'c'") != std::string::npos);
        CHECK(e.message().find("line 4") != std::string::npos);
    }
    CHECK_THROWS_AS(yaml::parse("\xFF"), DataException);
    yaml::ParseOptions shallow;
    shallow.max_depth = 8;
    CHECK_THROWS_AS(yaml::parse(std::string(50, '[') + std::string(50, ']'), shallow), DataException);
    CHECK_THROWS_AS(yaml::parse(std::string(100000, '[')), DataException);
}

FLYG_TEST(data_yaml_emit) {
    CHECK_EQ(yaml::emit(Value()), "null\n");
    CHECK_EQ(yaml::emit(Value(5)), "5\n");
    CHECK_EQ(yaml::emit(Value(2.0)), "2.0\n");
    CHECK_EQ(yaml::emit(Value("plain text")), "plain text\n");
    CHECK_EQ(yaml::emit(Value("123")), "\"123\"\n");
    CHECK_EQ(yaml::emit(Value("true")), "\"true\"\n");
    CHECK_EQ(yaml::emit(Value("")), "\"\"\n");
    CHECK_EQ(yaml::emit(Value("a: b")), "\"a: b\"\n");
    CHECK_EQ(yaml::emit(Value("line\nbreak")), "\"line\\nbreak\"\n");
    CHECK_EQ(yaml::emit(Value::array()), "[]\n");
    CHECK_EQ(yaml::emit(Value::object()), "{}\n");
    CHECK_EQ(yaml::emit(J(R"({"a":1,"b":[1,2],"c":{"d":null,"e":[]},"f":[{"x":1,"y":[3]},[4,5]]})")),
             "a: 1\nb:\n- 1\n- 2\nc:\n  d: null\n  e: []\nf:\n- x: 1\n  y:\n  - 3\n- - 4\n  - 5\n");
    yaml::EmitOptions o;
    o.indent = 4;
    o.document_marker = true;
    o.sort_keys = true;
    CHECK_EQ(yaml::emit(J(R"({"b":{"z":1,"a":2},"a":3})"), o), "---\na: 3\nb:\n    a: 2\n    z: 1\n");
    o.indent = 0;
    CHECK_THROWS_AS(yaml::emit(Value(1), o), DataException);
    CHECK_EQ(yaml::emit(Value(Bytes{std::byte{'h'}, std::byte{'i'}})), "!!binary aGk=\n");
    CHECK_EQ(yaml::emit(Value(std::numeric_limits<double>::infinity())), ".inf\n");
    CHECK_THROWS_AS(yaml::emit(Value("\xFF")), DataException);
}

FLYG_TEST(data_yaml_round_trip) {
    u64 seed = 777;
    auto rnd = [&] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<u32>(seed >> 33); };
    static const char* const words[] = {"a", "hello world", "", " lead", "trail ", "x: y", "# not comment", "- dash", "true", "null", "123",
                                        "1.5", "multi\nline", "tab\there", "quote\"s", "'single'", "é한😀", "---", "[x]", "{y}", "a #b", "key:", ":", "?",
                                        "&anchor", "*alias", "!tag", "|", ">", "%", "@", "`", "0x1f", ".inf", "~", "back\\slash", "colon:no-space"};
    std::function<Value(int)> gen = [&](int depth) -> Value {
        switch (rnd() % (depth > 3 ? 5 : 7)) {
            case 0: return Value();
            case 1: return Value(rnd() % 2 == 0);
            case 2: return Value(static_cast<i64>(rnd()) - 1000000000);
            case 3: return Value(static_cast<double>(rnd()) / 7.0);
            case 4: return Value(std::string(words[rnd() % (sizeof words / sizeof words[0])]));
            case 5: { Array a; for (u32 i = rnd() % 4; i > 0; --i) a.push_back(gen(depth + 1)); return Value(a); }
            default: { Object o; for (u32 i = rnd() % 4; i > 0; --i) o.insert_or_assign(words[rnd() % (sizeof words / sizeof words[0])], gen(depth + 1)); return Value(o); }
        }
    };
    for (int i = 0; i < 500; ++i) {
        const Value g = gen(0);
        const std::string y = yaml::emit(g);
        Value back;
        try {
            back = yaml::parse(y);
        } catch (const DataException& e) {
            flyg_test::report_failure(__FILE__, __LINE__, std::string(e.what()) + "\n--- YAML:\n" + y);
            continue;
        }
        ++flyg_test::check_count();
        if (!(back == g)) flyg_test::report_failure(__FILE__, __LINE__, "round trip mismatch\n--- YAML:\n" + y + "--- got: " + S(back) + "\n--- want: " + S(g));
    }
}
