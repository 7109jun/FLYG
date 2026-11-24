// flyG - data module tests: document database
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>
#include <flyg/system/filesystem.hpp>

#include <thread>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;

namespace {
Value J(std::string_view s) { return json::parse(s); }

std::vector<std::string> names(const std::vector<Value>& docs) {
    std::vector<std::string> out;
    for (const auto& d : docs) out.push_back(d.at("name").as_string());
    std::sort(out.begin(), out.end());
    return out;
}

void seed(Collection& c) {
    c.insert(J(R"({"name":"ann","age":31,"tags":["a","b"],"addr":{"city":"Seoul","zip":"04"}})"));
    c.insert(J(R"({"name":"bob","age":25,"tags":["b"],"addr":{"city":"Busan"}})"));
    c.insert(J(R"({"name":"cy","age":40,"tags":[],"addr":{"city":"Seoul"},"vip":true})"));
    c.insert(J(R"({"name":"dee","age":25.0,"items":[{"sku":"x","qty":2},{"sku":"y","qty":5}]})"));
    c.insert(J(R"({"name":"eve","age":"unknown"})"));
}
}  // namespace

FLYG_TEST(data_db_crud) {
    Database db;
    auto c = db.collection("users");
    const auto id = c.insert(J(R"({"name":"ann","age":31})"));
    CHECK_EQ(id.size(), 16u);
    auto d = c.get(id);
    CHECK(d.has_value());
    CHECK_EQ(d->at("_id").as_string(), id);
    CHECK_EQ(d->at("name").as_string(), "ann");
    CHECK(!c.get("nope"));
    const auto id2 = c.insert(J(R"({"_id":"custom","name":"bob"})"));
    CHECK_EQ(id2, "custom");
    CHECK_THROWS_AS(c.insert(J(R"({"_id":"custom"})")), DataException);
    CHECK_THROWS_AS(c.insert(Value(5)), DataException);
    CHECK_THROWS_AS(c.insert(J(R"({"_id":7})")), DataException);
    CHECK(c.update(id, J(R"({"age":32,"extra":{"a":1}})")));
    CHECK_EQ(c.get(id)->at("age").as_int(), 32);
    CHECK(c.update(id, J(R"({"extra":null})")));
    CHECK(!c.get(id)->contains("extra"));
    CHECK_THROWS_AS(c.update(id, J(R"({"_id":"x"})")), DataException);
    CHECK(c.replace(id, J(R"({"name":"ann2"})")));
    CHECK(!c.get(id)->contains("age"));
    CHECK_EQ(c.get(id)->at("_id").as_string(), id);
    CHECK(!c.replace("nope", J("{}")));
    CHECK(!c.update("nope", J("{}")));
    CHECK(c.erase(id));
    CHECK(!c.erase(id));
    CHECK_EQ(c.count(), 1u);
    auto ids = c.insert_many({J(R"({"name":"x"})"), J(R"({"name":"y"})")});
    CHECK_EQ(ids.size(), 2u);
    CHECK(ids[0] < ids[1]);
    CHECK_EQ(db.collection_names().size(), 1u);
    CHECK_THROWS_AS(db.collection(""), DataException);
}

FLYG_TEST(data_db_query_operators) {
    Database db;
    auto c = db.collection("u");
    seed(c);
    auto q = [&](std::string_view f) { return names(c.find(J(f))); };
    using V = std::vector<std::string>;
    CHECK(q(R"({"name":"ann"})") == V{"ann"});
    CHECK(q(R"({"age":25})") == (V{"bob", "dee"}));          // int and double compare numerically
    CHECK(q(R"({"age":{"$gt":25}})") == (V{"ann", "cy"}));   // strings are not numbers
    CHECK(q(R"({"age":{"$gte":25,"$lt":40}})") == (V{"ann", "bob", "dee"}));
    CHECK(q(R"({"age":{"$ne":25}})") == (V{"ann", "cy", "eve"}));
    CHECK(q(R"({"age":{"$in":[25,40]}})") == (V{"bob", "cy", "dee"}));
    CHECK(q(R"({"age":{"$nin":[25,40]}})") == (V{"ann", "eve"}));
    CHECK(q(R"({"vip":{"$exists":true}})") == V{"cy"});
    CHECK(q(R"({"vip":{"$exists":false}})").size() == 4);
    CHECK(q(R"({"addr.city":"Seoul"})") == (V{"ann", "cy"}));
    CHECK(q(R"({"addr.zip":{"$exists":true}})") == V{"ann"});
    CHECK(q(R"({"tags":"b"})") == (V{"ann", "bob"}));       // array-any
    CHECK(q(R"({"tags":{"$contains":"a"}})") == V{"ann"});
    CHECK(q(R"({"tags":{"$size":2}})") == V{"ann"});
    CHECK(q(R"({"tags":{"$size":0}})") == V{"cy"});
    CHECK(q(R"({"name":{"$prefix":"b"}})") == V{"bob"});
    CHECK(q(R"({"name":{"$suffix":"e"}})") == (V{"dee", "eve"}));
    CHECK(q(R"({"name":{"$contains":"e"}})") == (V{"dee", "eve"}));
    CHECK(q(R"({"age":{"$type":"string"}})") == V{"eve"});
    CHECK(q(R"({"age":{"$type":"number"}})").size() == 4);
    CHECK(q(R"({"items.sku":"y"})") == V{"dee"});
    CHECK(q(R"({"items.1.qty":5})") == V{"dee"});
    CHECK(q(R"({"items.qty":{"$gt":4}})") == V{"dee"});
    CHECK(q(R"({"$or":[{"name":"ann"},{"age":40}]})") == (V{"ann", "cy"}));
    CHECK(q(R"({"$and":[{"age":{"$gt":20}},{"addr.city":"Seoul"}]})") == (V{"ann", "cy"}));
    CHECK(q(R"({"$nor":[{"addr.city":"Seoul"},{"age":25}]})") == V{"eve"});
    CHECK(q(R"({"age":{"$not":{"$gt":30}}})") == (V{"bob", "dee", "eve"}));
    CHECK_EQ(c.count(J("{}")), 5u);
    CHECK_EQ(c.count(J(R"({"addr.city":"Seoul"})")), 2u);
    CHECK(c.find_one(J(R"({"name":"cy"})")).has_value());
    CHECK(!c.find_one(J(R"({"name":"zz"})")).has_value());
    // invalid queries are reported even if no document would reach the bad part
    CHECK_THROWS_AS(c.find(J(R"({"age":{"$bogus":1}})")), DataException);
    CHECK_THROWS_AS(c.find(J(R"({"$or":[{"name":"ann"},{"x":{"$bogus":1}}]})")), DataException);
    CHECK_THROWS_AS(c.find(J(R"({"$what":1})")), DataException);
    CHECK_THROWS_AS(c.find(J(R"({"a":{"$in":5}})")), DataException);
    CHECK_THROWS_AS(c.find(J(R"({"a":{"$gt":1,"b":2}})")), DataException);
    CHECK_THROWS_AS(c.find(J(R"({"a":{"$type":"nope"}})")), DataException);
    CHECK_THROWS_AS(c.find(Value(5)), DataException);
}

FLYG_TEST(data_db_find_options) {
    Database db;
    auto c = db.collection("u");
    seed(c);
    FindOptions o;
    o.sort = {{"age", -1}, {"name", 1}};
    o.limit = 3;
    auto r = c.find(J(R"({"age":{"$gte":0}})"), o);
    CHECK_EQ(r.size(), 3u);
    CHECK_EQ(r[0].at("name").as_string(), "cy");
    CHECK_EQ(r[1].at("name").as_string(), "ann");
    CHECK_EQ(r[2].at("name").as_string(), "bob");
    o.skip = 1;
    o.limit = 1;
    CHECK_EQ(c.find(J(R"({"age":{"$gte":0}})"), o)[0].at("name").as_string(), "ann");
    o.skip = 100;
    CHECK(c.find(J("{}"), o).empty());
    // missing values sort first ascending
    FindOptions s;
    s.sort = {{"vip", 1}, {"name", 1}};
    CHECK_EQ(c.find(J("{}"), s).back().at("name").as_string(), "cy");
    FindOptions p;
    p.include = {"name", "addr.city"};
    auto pr = c.find(J(R"({"name":"ann"})"), p);
    CHECK_EQ(pr[0].size(), 3u);
    CHECK_EQ(pr[0].at_path("addr.city").as_string(), "Seoul");
    CHECK(!pr[0].at("addr").contains("zip"));
    FindOptions e;
    e.exclude = {"tags", "addr"};
    auto er = c.find(J(R"({"name":"ann"})"), e);
    CHECK(!er[0].contains("tags") && er[0].contains("age"));
}

FLYG_TEST(data_db_indexes_and_planner) {
    Database db;
    auto c = db.collection("u");
    seed(c);
    CHECK_EQ(c.explain(J(R"({"name":"ann"})"))["plan"].as_string(), "scan");
    c.create_index("name");
    c.create_index("age");
    c.create_index("tags");
    c.create_index("addr.city");
    auto ex = c.explain(J(R"({"name":"ann"})"));
    CHECK_EQ(ex["plan"].as_string(), "index");
    CHECK_EQ(ex["index"].as_string(), "name");
    CHECK_EQ(ex["candidates"].as_int(), 1);
    CHECK_EQ(c.explain(J(R"({"age":{"$gt":26}})"))["op"].as_string(), "range");
    CHECK_EQ(c.explain(J(R"({"age":{"$in":[25,40]}})"))["op"].as_string(), "in");
    CHECK_EQ(c.explain(J(R"({"vip":true})"))["plan"].as_string(), "scan");
    CHECK_EQ(c.explain(J(R"({"$and":[{"vip":true},{"name":"cy"}]})"))["index"].as_string(), "name");
    CHECK_EQ(c.indexes().size(), 4u);
    using V = std::vector<std::string>;
    // index-backed results must equal what a scan gives
    for (auto f : {R"({"age":{"$gt":26}})", R"({"age":{"$lte":25}})", R"({"age":{"$gte":25,"$lt":40}})", R"({"age":{"$in":[25,40,"unknown"]}})",
                   R"({"tags":"b"})", R"({"addr.city":"Seoul"})", R"({"name":{"$in":["ann","eve","none"]}})", R"({"age":25.0})",
                   R"({"age":{"$gt":"a"}})", R"({"name":"ann","age":{"$gt":50}})"}) {
        auto with = names(c.find(J(f)));
        c.drop_index("age"); c.drop_index("name"); c.drop_index("tags"); c.drop_index("addr.city");
        auto without = names(c.find(J(f)));
        CHECK(with == without);
        c.create_index("name"); c.create_index("age"); c.create_index("tags"); c.create_index("addr.city");
    }
    // maintenance on update/erase/replace
    auto id = c.find_one(J(R"({"name":"bob"})"))->at("_id").as_string();
    c.update(id, J(R"({"age":99,"tags":["z"]})"));
    CHECK(names(c.find(J(R"({"age":25})"))) == V{"dee"});
    CHECK(names(c.find(J(R"({"age":99})"))) == V{"bob"});
    CHECK(names(c.find(J(R"({"tags":"b"})"))) == V{"ann"});
    CHECK(names(c.find(J(R"({"tags":"z"})"))) == V{"bob"});
    c.erase(id);
    CHECK(c.find(J(R"({"age":99})")).empty());
    CHECK(!c.drop_index("nope"));
}

FLYG_TEST(data_db_unique_index) {
    Database db;
    auto c = db.collection("u");
    c.insert(J(R"({"email":"a@x","n":1})"));
    c.insert(J(R"({"email":"b@x","n":2})"));
    c.create_index("email", true);
    CHECK_THROWS_AS(c.insert(J(R"({"email":"a@x"})")), DataException);
    CHECK_EQ(c.count(), 2u);
    auto id = c.insert(J(R"({"email":"c@x"})"));
    CHECK_THROWS_AS(c.update(id, J(R"({"email":"b@x"})")), DataException);
    CHECK_EQ(c.get(id)->at("email").as_string(), "c@x");
    CHECK(c.update(id, J(R"({"email":"c@x","n":3})")));  // same value on same doc is fine
    CHECK(c.update(id, J(R"({"email":"d@x"})")));
    c.insert(J(R"({"email":"c@x"})"));                    // freed by the update
    CHECK_THROWS_AS(c.create_index("email", false), DataException);
    // creating a unique index over duplicates fails and leaves nothing behind
    auto d = db.collection("v");
    d.insert(J(R"({"k":1})"));
    d.insert(J(R"({"k":1})"));
    CHECK_THROWS_AS(d.create_index("k", true), DataException);
    CHECK(d.indexes().empty());
    d.create_index("k", false);
}

FLYG_TEST(data_db_transactions) {
    Database db;
    auto c = db.collection("acct");
    c.create_index("owner", true);
    c.insert(J(R"({"_id":"a","owner":"ann","bal":100})"));
    c.insert(J(R"({"_id":"b","owner":"bob","bal":50})"));
    db.transaction([&] {
        c.update("a", J(R"({"bal":70})"));
        c.update("b", J(R"({"bal":80})"));
        CHECK_EQ(c.get("a")->at("bal").as_int(), 70);  // reads see own writes
        CHECK_EQ(c.count(J(R"({"bal":80})")), 1u);
    });
    CHECK_EQ(c.get("b")->at("bal").as_int(), 80);
    // rollback restores documents and indexes
    try {
        db.transaction([&] {
            c.update("a", J(R"({"bal":0,"owner":"zed"})"));
            c.erase("b");
            c.insert(J(R"({"_id":"c","owner":"cy"})"));
            throw std::runtime_error("boom");
        });
        CHECK(false);
    } catch (const std::runtime_error&) {
    }
    CHECK_EQ(c.get("a")->at("bal").as_int(), 70);
    CHECK(c.get("b").has_value());
    CHECK(!c.get("c").has_value());
    CHECK_EQ(c.count(J(R"({"owner":"ann"})")), 1u);
    CHECK_EQ(c.count(J(R"({"owner":"zed"})")), 0u);
    CHECK_EQ(c.explain(J(R"({"owner":"ann"})"))["candidates"].as_int(), 1);
    // a failing operation aborts the whole (flat) transaction
    CHECK_THROWS_AS(db.transaction([&] {
        c.update("a", J(R"({"bal":1})"));
        c.insert(J(R"({"owner":"bob"})"));  // unique violation
    }), DataException);
    CHECK_EQ(c.get("a")->at("bal").as_int(), 70);
    // nested transactions join the outer one; return values pass through
    const auto n = db.transaction([&] {
        db.transaction([&] { c.update("a", J(R"({"bal":5})")); });
        return c.count();
    });
    CHECK_EQ(n, 2u);
    // schema changes roll back too
    try {
        db.transaction([&] {
            c.create_index("bal");
            db.collection("fresh").insert(J(R"({"x":1})"));
            throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
    }
    CHECK_EQ(c.indexes().size(), 1u);
    CHECK_EQ(db.collection_names().size(), 1u);
    CHECK(db.collection("fresh").get("0000000000000001") == std::nullopt);
    CHECK_EQ(db.collection_names().size(), 1u);
}

FLYG_TEST(data_db_bulk_ops_and_drop) {
    Database db;
    auto c = db.collection("u");
    c.create_index("age");
    seed(c);
    CHECK_EQ(c.update_many(J(R"({"age":25})"), J(R"({"flag":true})")), 2u);
    CHECK_EQ(c.count(J(R"({"flag":true})")), 2u);
    CHECK_EQ(c.erase_many(J(R"({"addr.city":"Seoul"})")), 2u);
    CHECK_EQ(c.count(), 3u);
    CHECK_EQ(c.erase_many(J(R"({"nothing":1})")), 0u);
    CHECK(db.drop_collection("u"));
    CHECK(!db.drop_collection("u"));
    CHECK(db.collection_names().empty());
    CHECK_EQ(db.collection("u").count(), 0u);
}

FLYG_TEST(data_db_persistence) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    std::string id1;
    {
        Database db(p);
        auto c = db.collection("users");
        c.create_index("email", true);
        c.create_index("age");
        id1 = c.insert(J(R"({"email":"a@x","age":30,"bin":"x"})"));
        c.insert(J(R"({"email":"b@x","age":20})"));
        c.insert(J(R"({"email":"c@x","age":30})"));
        c.erase(c.find_one(J(R"({"email":"c@x"})"))->at("_id").as_string());
        db.collection("other").insert(J(R"({"v":[1,2,3]})"));
    }
    {
        Database db(p);
        auto names_ = db.collection_names();
        CHECK_EQ(names_.size(), 2u);
        auto c = db.collection("users");
        CHECK_EQ(c.count(), 2u);
        CHECK_EQ(c.indexes().size(), 2u);
        CHECK_EQ(c.explain(J(R"({"email":"a@x"})"))["plan"].as_string(), "index");
        CHECK_EQ(c.get(id1)->at("age").as_int(), 30);
        CHECK_THROWS_AS(c.insert(J(R"({"email":"a@x"})")), DataException);  // unique index survived
        const auto id3 = c.insert(J(R"({"email":"d@x"})"));
        CHECK(id3 > id1);                                                    // counter survived, no id reuse
        CHECK_EQ(c.count(J(R"({"age":{"$gte":20}})")), 2u);
        CHECK(db.drop_collection("other"));
    }
    {
        Database db(p);
        CHECK_EQ(db.collection_names().size(), 1u);
        db.compact();
    }
    Database db(p);
    CHECK_EQ(db.collection("users").count(), 3u);
}

FLYG_TEST(data_db_rollback_not_persisted) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    {
        Database db(p);
        auto c = db.collection("u");
        c.insert(J(R"({"_id":"k","v":1})"));
        try {
            db.transaction([&] { c.update("k", J(R"({"v":2})")); throw std::runtime_error("x"); });
        } catch (const std::runtime_error&) {}
    }
    Database db(p);
    CHECK_EQ(db.collection("u").get("k")->at("v").as_int(), 1);
}

FLYG_TEST(data_db_concurrency) {
    Database db;
    auto c = db.collection("n");
    c.create_index("t");
    c.insert(J(R"({"_id":"counter","v":0})"));
    constexpr int threads = 4, per = 100;
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back([&, i] {
            auto cc = db.collection("n");
            for (int j = 0; j < per; ++j) {
                db.transaction([&] {
                    const i64 v = cc.get("counter")->at("v").as_int();
                    cc.update("counter", Value(Object{{"v", v + 1}}));
                    cc.insert(Value(Object{{"t", i}}));
                });
                (void)cc.count(J(R"({"t":1})"));
            }
        });
    for (auto& t : ts) t.join();
    CHECK_EQ(c.get("counter")->at("v").as_int(), threads * per);
    CHECK_EQ(c.count(), static_cast<usize>(threads * per + 1));
    CHECK_EQ(c.count(J(R"({"t":2})")), static_cast<usize>(per));
}
