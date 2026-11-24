// flyG - data module tests: CSV and XML
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>

#include <sstream>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

// ---- CSV -------------------------------------------------------------------------------------
FLYG_TEST(data_csv_rows) {
    auto rows = csv::parse_rows("a,b,c\n1,2,3\n");
    CHECK_EQ(rows.size(), usize{2});
    CHECK((rows[0] == csv::Row{"a", "b", "c"}));
    CHECK((rows[1] == csv::Row{"1", "2", "3"}));

    rows = csv::parse_rows("a,b\r\nc,d\re,f");  // CRLF, CR, no final newline
    CHECK_EQ(rows.size(), usize{3});
    CHECK((rows[2] == csv::Row{"e", "f"}));

    rows = csv::parse_rows("\"x,y\",\"say \"\"hi\"\"\",\"multi\nline\"\n");
    CHECK_EQ(rows.size(), usize{1});
    CHECK((rows[0] == csv::Row{"x,y", "say \"hi\"", "multi\nline"}));

    rows = csv::parse_rows("a,,c,\n,\n");  // empty fields incl. trailing
    CHECK((rows[0] == csv::Row{"a", "", "c", ""}));
    CHECK((rows[1] == csv::Row{"", ""}));
    rows = csv::parse_rows("a,b,");  // trailing delimiter at EOF
    CHECK((rows[0] == csv::Row{"a", "b", ""}));

    rows = csv::parse_rows("a\n\n\nb\n");
    CHECK_EQ(rows.size(), usize{2});
    csv::Options keep;
    keep.skip_empty_lines = false;
    rows = csv::parse_rows("a\n\nb\n", keep);
    CHECK_EQ(rows.size(), usize{3});

    csv::Options semi;
    semi.delimiter = ';';
    rows = csv::parse_rows("a;\"b;c\";d\n", semi);
    CHECK((rows[0] == csv::Row{"a", "b;c", "d"}));
    csv::Options trim;
    trim.trim_unquoted = true;
    rows = csv::parse_rows("  a  ,  \" b \"  ,c\n", trim);
    CHECK((rows[0] == csv::Row{"a", " b ", "c"}));
    rows = csv::parse_rows("\xEF\xBB\xBF" "x,y\n");
    CHECK((rows[0] == csv::Row{"x", "y"}));
    CHECK(csv::parse_rows("").empty());

    CHECK_THROWS_AS(csv::parse_rows("\"abc"), DataException);
    CHECK_THROWS_AS(csv::parse_rows("\"a\"b,c"), DataException);
    CHECK_THROWS_AS(csv::parse_rows("ab\"c,d"), DataException);
    csv::Options lax;
    lax.strict_quotes = false;
    CHECK_EQ(csv::parse_rows("ab\"c,d", lax)[0][0], "ab\"c");
    CHECK_EQ(csv::parse_rows("\"a\"b,c", lax)[0][0], "ab");
    csv::Options bad;
    bad.delimiter = '"';
    CHECK_THROWS_AS(csv::RowParser("x", bad), DataException);
}

FLYG_TEST(data_csv_table_and_write) {
    const Value t = csv::parse("name,age,ok\nAda,36,true\nBob,,false\n");
    CHECK_EQ(t.size(), usize{2});
    CHECK_EQ(t.at(0).at("name").as_string(), "Ada");
    CHECK_EQ(t.at(0).at("age").as_string(), "36");  // no inference by default
    csv::TableOptions inf;
    inf.infer_types = true;
    const Value ti = csv::parse("name,age,ratio,ok,zip\nAda,36,1.5,true,007\nBob,,2e3,false,+1\n", inf);
    CHECK_EQ(ti.at(0).at("age").as_int(), i64{36});
    CHECK_EQ(ti.at(0).at("ratio").as_double(), 1.5);
    CHECK(ti.at(0).at("ok").as_bool());
    CHECK_EQ(ti.at(0).at("zip").as_string(), "007");
    CHECK(ti.at(1).at("age").is_null());
    CHECK_EQ(ti.at(1).at("ratio").as_double(), 2000.0);
    CHECK_EQ(ti.at(1).at("zip").as_string(), "+1");

    csv::TableOptions raw;
    raw.header = false;
    const Value tr = csv::parse("1,2\n3,4\n", raw);
    CHECK(tr.at(1).is_array());
    CHECK_EQ(tr.at(1).at(1).as_string(), "4");
    CHECK_THROWS_AS(csv::parse("a,b\n1\n"), DataException);
    csv::TableOptions loose;
    loose.strict_columns = false;
    CHECK(csv::parse("a,b\n1\n", loose).at(0).at("b").is_null());
    CHECK_THROWS_AS(csv::parse("a,a\n1,2\n"), DataException);

    CHECK_EQ(csv::write_row({"a", "b,c", "d\"e", " f", ""}), "a,\"b,c\",\"d\"\"e\",\" f\",");
    CHECK_EQ(csv::write_row({""}), "\"\"");  // distinguishable from a blank line
    CHECK_EQ(csv::write_rows({{"1", "2"}, {"3", "4"}}, {}, "\n"), "1,2\n3,4\n");
    const Value tbl = json::parse(R"([{"n":"x","v":1},{"n":"y","w":2.5,"v":null},{"n":"z, q","v":true}])");
    CHECK_EQ(csv::write(tbl, {}, true, "\n"), "n,v,w\nx,1,\ny,,2.5\n\"z, q\",true,\n");
    CHECK_THROWS_AS(csv::write(json::parse(R"([{"a":[1]}])")), DataException);
    CHECK_THROWS_AS(csv::write(json::parse(R"([{"a":1},[1]])")), DataException);

    // round trip of nasty content
    csv::Row nasty = {"plain", "with,comma", "with \"quote\"", "multi\r\nline", " lead", "trail ", "", "한글", "a\tb"};
    const auto back = csv::parse_rows(csv::write_row(nasty) + "\n");
    CHECK_EQ(back.size(), usize{1});
    CHECK(back[0] == nasty);
}

FLYG_TEST(data_csv_stream) {
    std::istringstream in("id,text\n1,\"a\nb\"\n2,plain\r\n3,\"q\"\"q\"\n");
    std::vector<csv::Row> got;
    const usize n = csv::read_stream(in, [&](const csv::Row& r) { got.push_back(r); return true; });
    CHECK_EQ(n, usize{4});
    CHECK_EQ(got[1][1], "a\nb");
    CHECK_EQ(got[3][1], "q\"q");
    std::istringstream stop("1\n2\n3\n");
    CHECK_EQ(csv::read_stream(stop, [](const csv::Row&) { return false; }), usize{1});
    std::istringstream bad("1,\"open\n");
    CHECK_THROWS_AS(csv::read_stream(bad, [](const csv::Row&) { return true; }), DataException);

    // 100k rows streamed
    std::string big;
    for (int i = 0; i < 100000; ++i) big += std::to_string(i) + ",row\n";
    std::istringstream bin(big);
    usize count = 0;
    csv::read_stream(bin, [&](const csv::Row& r) { count += r.size(); return true; });
    CHECK_EQ(count, usize{200000});
}

// ---- XML -------------------------------------------------------------------------------------
FLYG_TEST(data_xml_parse_basic) {
    const auto d = xml::parse(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<!-- lead --><?xml-stylesheet href=\"a.css\"?>\n"
        "<!DOCTYPE note [ <!ENTITY x \"boom\"> ]>\n"
        "<root a=\"1\" b='two &amp; three'>\n"
        "  <child id=\"c1\">Hello &lt;world&gt; &#65;&#x42;&#x1F600;</child>\n"
        "  <empty/>\n"
        "  <data><![CDATA[<raw> & stuff]]></data>\n"
        "  <!-- inner -->\n"
        "  <mixed>pre<b>bold</b>post</mixed>\n"
        "</root>\n<!-- tail -->");
    CHECK_EQ(d.version, "1.0");
    CHECK(d.standalone.value());
    CHECK_EQ(d.prolog.size(), usize{2});
    CHECK_EQ(d.epilog.size(), usize{1});
    CHECK_EQ(d.root.name, "root");
    CHECK_EQ(d.root.attribute("a")->c_str(), std::string("1"));
    CHECK_EQ(*d.root.attribute("b"), "two & three");
    CHECK(d.root.attribute("zzz") == nullptr);
    CHECK_EQ(d.root.attribute_or("zzz", "dflt"), "dflt");
    const auto* c = d.root.first_child("child");
    CHECK(c != nullptr);
    CHECK_EQ(c->inner_text(), "Hello <world> AB😀");
    CHECK(d.root.first_child("empty")->children.empty());
    CHECK_EQ(d.root.first_child("data")->inner_text(), "<raw> & stuff");
    CHECK_EQ(d.root.first_child("mixed")->inner_text(), "prebold" "post");
    CHECK_EQ(d.root.first_child("mixed")->children.size(), usize{3});
    CHECK_EQ(d.root.children_named("child").size(), usize{1});
    // whitespace-only text between elements is dropped, comments kept
    usize comments = 0, texts = 0;
    for (const auto& n : d.root.children) {
        comments += n.kind == xml::Node::Kind::comment;
        texts += n.kind == xml::Node::Kind::text;
    }
    CHECK_EQ(comments, usize{1});
    CHECK_EQ(texts, usize{0});
    // DOCTYPE entities are NOT expanded
    CHECK_THROWS_AS(xml::parse("<!DOCTYPE a [<!ENTITY x \"boom\">]><a>&x;</a>"), DataException);

    xml::ParseOptions keep;
    keep.keep_whitespace = true;
    keep.keep_comments = false;
    const auto k = xml::parse("<a> <b/> <!--c--> </a>", keep);
    CHECK_EQ(k.root.children.size(), usize{3});  // " ", b, "  " (text around the dropped comment is merged)
}

FLYG_TEST(data_xml_errors) {
    const char* bad[] = {"",
                         "text",
                         "<a>",
                         "<a></b>",
                         "<a><b></a></b>",
                         "<a x=1/>",
                         "<a x=\"1\" x=\"2\"/>",
                         "<a x=\"<\"/>",
                         "<a>&unknown;</a>",
                         "<a>&#0;</a>",
                         "<a>&#x110000;</a>",
                         "<a>&amp</a>",
                         "<a/><b/>",
                         "<a><!-- -- --></a>",
                         "<a><![CDATA[x</a>",
                         "<a>]]></a>",
                         "<1a/>",
                         "<a b/>",
                         "<a b=\"1\"c=\"2\"/>",
                         "<?xml version=\"1.0\" encoding=\"UTF-16\"?><a/>",
                         "<a>\x01</a>",
                         "<a/>junk",
                         "<a><?xml x?></a>",
                         "<!DOCTYPE a><a/><!DOCTYPE b>",
                         "<a \xFF/>"};
    for (const char* b : bad) {
        bool threw = false;
        try {
            (void)xml::parse(b);
        } catch (const DataException&) {
            threw = true;
        }
        if (!threw) flyg_test::report_failure(__FILE__, __LINE__, std::string("should have failed: ") + b);
        ++flyg_test::check_count();
    }
    try {
        (void)xml::parse("<a>\n  <b>\n</a>");
        CHECK(false);
    } catch (const DataException& e) {
        CHECK(e.message().find("line 3") != std::string::npos);
    }
    xml::ParseOptions shallow;
    shallow.max_depth = 5;
    CHECK_THROWS_AS(xml::parse("<a><a><a><a><a><a/></a></a></a></a></a>", shallow), DataException);
    CHECK_THROWS_AS(xml::parse(std::string(100000, '<') ), DataException);
    std::string deep;
    for (int i = 0; i < 100000; ++i) deep += "<a>";
    CHECK_THROWS_AS(xml::parse(deep), DataException);  // default depth limit, no stack overflow
}

FLYG_TEST(data_xml_write_and_round_trip) {
    xml::Node root = xml::Node::element("root");
    root.set_attribute("title", "a \"quoted\" <value> & more\nline");
    root.set_attribute("title", "a \"quoted\" <value> & more\nline");  // replace, not duplicate
    CHECK_EQ(root.attributes.size(), usize{1});
    xml::Node& item = root.add_child(xml::Node::element("item"));
    item.add_child(xml::Node::make_text("1 < 2 && 3 > 2 \r"));
    root.add_child(xml::Node::element("empty"));
    xml::Node cd;
    cd.kind = xml::Node::Kind::cdata;
    cd.text = "<keep>";
    root.add_child(xml::Node::element("c")).add_child(cd);

    xml::WriteOptions compact;
    compact.xml_declaration = false;
    const std::string s = xml::write(root, compact);
    CHECK_EQ(s, "<root title=\"a &quot;quoted&quot; &lt;value&gt; &amp; more&#10;line\"><item>1 &lt; 2 &amp;&amp; 3 &gt; 2 &#13;</item><empty/><c><![CDATA[<keep>]]></c></root>");
    const auto back = xml::parse(s);
    CHECK_EQ(*back.root.attribute("title"), "a \"quoted\" <value> & more\nline");  // &#10; survives
    CHECK_EQ(back.root.first_child("item")->inner_text(), "1 < 2 && 3 > 2 \r");

    xml::WriteOptions pretty;
    pretty.indent = 2;
    const std::string p = xml::write(root, pretty);
    CHECK(p.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<root", 0) == 0);
    CHECK(p.find("\n  <empty/>\n") != std::string::npos);
    CHECK(p.find("<item>1 &lt;") != std::string::npos);  // mixed/text content left untouched
    const auto pb = xml::parse(p);
    CHECK_EQ(pb.root.children.size(), root.children.size());

    xml::Node bad = xml::Node::element("1bad");
    CHECK_THROWS_AS(xml::write(bad), DataException);
    xml::Node badc;
    badc.kind = xml::Node::Kind::comment;
    badc.text = "a--b";
    CHECK_THROWS_AS(xml::write(badc), DataException);

    xml::Document doc;
    doc.root = xml::Node::element("r");
    doc.standalone = false;
    xml::Node pi;
    pi.kind = xml::Node::Kind::processing_instruction;
    pi.name = "target";
    pi.text = "data";
    doc.prolog.push_back(pi);
    const std::string ds = xml::write(doc);
    CHECK_EQ(ds, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?><?target data?><r/>");
    const auto dd = xml::parse(ds);
    CHECK_EQ(dd.prolog.size(), usize{1});
    CHECK(dd.standalone.has_value() && !*dd.standalone);
}

FLYG_TEST(data_xml_value_bridge) {
    const auto d = xml::parse(R"(<lib name="L"><book id="1"><t>A</t></book><book id="2"><t>B</t></book><note>hi</note><nil/></lib>)");
    const Value v = xml::to_value(d.root);
    CHECK_EQ(v.at("@name").as_string(), "L");
    CHECK(v.at("book").is_array());
    CHECK_EQ(v.at("book").size(), usize{2});
    CHECK_EQ(v.at_path("book[1].t").as_string(), "B");
    CHECK_EQ(v.at_path("book[0].@id").as_string(), "1");
    CHECK_EQ(v.at("note").as_string(), "hi");
    CHECK(v.at("nil").is_null());

    const xml::Node n = xml::from_value("lib", v);
    xml::WriteOptions w;
    w.xml_declaration = false;
    const auto again = xml::parse(xml::write(n, w));
    CHECK(xml::to_value(again.root) == v);

    const Value mixed = xml::to_value(xml::parse("<p a=\"x\">text<b/></p>").root);
    CHECK_EQ(mixed.at("#text").as_string(), "text");
    CHECK_THROWS_AS(xml::from_value("x", Value::array({1})), DataException);
    CHECK_THROWS_AS(xml::from_value("x", json::parse(R"({"@a":[1]})")), DataException);
    CHECK_EQ(xml::from_value("n", Value(42)).inner_text(), "42");
}
