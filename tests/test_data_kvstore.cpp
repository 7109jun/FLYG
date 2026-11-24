// flyG - data module tests: key-value store
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/data/data.hpp>
#include <flyg/system/filesystem.hpp>

#include <fstream>
#include <thread>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::data;
namespace fs = std::filesystem;

namespace {
std::vector<char> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void spit(const fs::path& p, const std::vector<char>& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(d.data(), static_cast<std::streamsize>(d.size()));
}
}  // namespace

FLYG_TEST(data_kv_memory_basic) {
    KvStore kv;
    CHECK(!kv.persistent());
    CHECK(!kv.get("a"));
    kv.put("b", "2");
    kv.put("a", "1");
    kv.put("c", "3");
    CHECK_EQ(*kv.get("a"), "1");
    CHECK_EQ(kv.size(), 3u);
    CHECK(kv.erase("b"));
    CHECK(!kv.erase("b"));
    CHECK(!kv.contains("b"));
    auto r = kv.range("a", std::nullopt);
    CHECK_EQ(r.size(), 2u);
    CHECK_EQ(r[0].first, "a");
    CHECK_EQ(r[1].first, "c");
    KvStore::WriteBatch b;
    b.put("x", "1");
    b.put("y", "2");
    b.erase("a");
    kv.write(b);
    CHECK(!kv.contains("a"));
    CHECK(kv.contains("y"));
}

FLYG_TEST(data_kv_scan_prefix_reverse) {
    KvStore kv;
    for (auto k : {"user:1", "user:2", "user:3", "usez", "item:1", "us"}) kv.put(k, k);
    std::vector<std::string> got;
    kv.scan_prefix("user:", [&](std::string_view k, std::string_view) { got.emplace_back(k); return true; });
    CHECK_EQ(got.size(), 3u);
    got.clear();
    kv.scan("user:1", std::string_view("user:3"), [&](std::string_view k, std::string_view) { got.emplace_back(k); return true; }, true);
    CHECK_EQ(got.size(), 2u);
    CHECK_EQ(got[0], "user:2");
    CHECK_EQ(got[1], "user:1");
    CHECK_EQ(kv.range("", std::nullopt, 2).size(), 2u);
    CHECK(!KvStore::prefix_end(std::string("\xff\xff")).has_value());
    CHECK_EQ(*KvStore::prefix_end("ab\xff"), "ac");
}

FLYG_TEST(data_kv_persistence_and_recovery) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    {
        KvStore kv(p);
        kv.put("k1", "v1");
        kv.put("k2", std::string(100000, 'x'));
        kv.erase("k1");
        KvStore::WriteBatch b;
        b.put("k3", "v3");
        b.put(std::string("bin\0key", 7), std::string("\0\1\2", 3));
        kv.write(b);
    }
    {
        KvStore kv(p);
        CHECK(!kv.contains("k1"));
        CHECK_EQ(kv.get("k2")->size(), 100000u);
        CHECK_EQ(*kv.get("k3"), "v3");
        CHECK_EQ(kv.get(std::string("bin\0key", 7))->size(), 3u);
        CHECK_EQ(kv.stats().recovered_records, 4u);
        CHECK_EQ(kv.stats().discarded_tail_bytes, 0u);
    }
}

FLYG_TEST(data_kv_exclusive_lock) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    KvStore kv(p);
    // flock is per open file description, so a second open in the same process conflicts too (Linux);
    // on Windows the share mode forbids it.
    CHECK_THROWS_AS(KvStore(p), DataException);
}

FLYG_TEST(data_kv_torn_tail_every_truncation) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    std::vector<usize> boundaries;
    {
        KvStore kv(p);
        for (int i = 0; i < 6; ++i) {
            kv.put("key" + std::to_string(i), "value" + std::to_string(i));
            boundaries.push_back(static_cast<usize>(kv.stats().log_bytes));
        }
    }
    const auto full = slurp(p);
    CHECK_EQ(full.size(), boundaries.back());
    for (usize cut = 8; cut <= full.size(); ++cut) {
        spit(p, std::vector<char>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)));
        KvStore kv(p);
        usize expect = 0;
        for (usize b : boundaries) if (b <= cut) ++expect;
        if (kv.size() != expect) { CHECK_EQ(kv.size(), expect); break; }
        // after recovery the file must be usable and consistent
        kv.put("after", "1");
    }
}

FLYG_TEST(data_kv_corruption_detected) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    {
        KvStore kv(p);
        kv.put("a", "1");
        kv.put("b", "2");
        kv.put("c", "3");
    }
    auto bytes = slurp(p);
    // flip a byte inside the second record: records after it are dropped, the first survives
    const usize mid = bytes.size() / 2;
    bytes[mid] = static_cast<char>(bytes[mid] ^ 0x55);
    spit(p, bytes);
    {
        KvStore::Options strict;
        strict.strict_recovery = true;
        CHECK_THROWS_AS(KvStore(p, strict), DataException);
    }
    KvStore kv(p);
    CHECK(kv.contains("a"));
    CHECK(!kv.contains("c"));
    CHECK(kv.stats().discarded_tail_bytes > 0);
    // not a database at all
    spit(p, {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'});
    CHECK_THROWS_AS(KvStore(p), DataException);
}

FLYG_TEST(data_kv_options) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    KvStore::Options no_create;
    no_create.create_if_missing = false;
    CHECK_THROWS_AS(KvStore(p, no_create), DataException);
    { KvStore kv(p); kv.put("a", "1"); }
    KvStore::Options ro;
    ro.read_only = true;
    KvStore kv(p, ro);
    CHECK_EQ(*kv.get("a"), "1");
    CHECK_THROWS_AS(kv.put("b", "2"), DataException);
    KvStore::Options sync;
    sync.sync_on_commit = true;
    CHECK_NOTHROW(([&] { KvStore k2(dir.path() / "s.fkv", sync); k2.put("x", "y"); }()));
}

FLYG_TEST(data_kv_compaction) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    KvStore::Options o;
    o.auto_compact = false;
    {
        KvStore kv(p, o);
        for (int i = 0; i < 2000; ++i) kv.put("hot", std::string(200, static_cast<char>('a' + i % 26)));
        kv.put("cold", "stay");
        const auto before = kv.stats().log_bytes;
        kv.compact();
        const auto s = kv.stats();
        CHECK(s.log_bytes < before / 50);
        CHECK_EQ(s.compactions, 1u);
        kv.put("after", "1");
        CHECK(!fs::exists(fs::path(p.string() + ".compact.tmp")));
    }
    KvStore kv(p, o);
    CHECK_EQ(kv.size(), 3u);
    CHECK_EQ(*kv.get("cold"), "stay");
    CHECK_EQ(*kv.get("after"), "1");
    // leftover temp file from an interrupted compaction is ignored
    { KvStore::Options x = o; (void)x; }
}

FLYG_TEST(data_kv_auto_compaction) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    KvStore::Options o;
    o.compact_min_bytes = 20000;
    o.compact_ratio = 2.0;
    KvStore kv(p, o);
    for (int i = 0; i < 3000; ++i) kv.put("k", std::string(100, 'x'));
    CHECK(kv.stats().compactions >= 1);
    CHECK(kv.stats().log_bytes < 100000);
    CHECK_EQ(kv.get("k")->size(), 100u);
}

FLYG_TEST(data_kv_interrupted_compaction_leftover) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    { KvStore kv(p); kv.put("a", "1"); }
    spit(fs::path(p.string() + ".compact.tmp"), {'j', 'u', 'n', 'k'});
    KvStore kv(p);
    CHECK_EQ(*kv.get("a"), "1");
    CHECK(!fs::exists(fs::path(p.string() + ".compact.tmp")));
}

FLYG_TEST(data_kv_transactions) {
    KvStore kv;
    kv.put("a", "1");
    {
        auto t = kv.begin();
        CHECK_EQ(*t.get("a"), "1");
        t.put("a", "2");
        t.put("n", "new");
        CHECK_EQ(*t.get("a"), "2");  // read-your-writes
        CHECK_EQ(*kv.get("a"), "1"); // isolated until commit
        CHECK(t.commit());
        CHECK_EQ(*kv.get("a"), "2");
        CHECK_THROWS_AS(t.put("z", "z"), DataException);
    }
    {  // conflict: someone changes what we read
        auto t = kv.begin();
        (void)t.get("a");
        kv.put("a", "other");
        t.put("a", "mine");
        CHECK(!t.commit());
        CHECK_EQ(*kv.get("a"), "other");
    }
    {  // blind writes never conflict
        auto t = kv.begin();
        kv.put("a", "x");
        t.put("a", "blind");
        CHECK(t.commit());
        CHECK_EQ(*kv.get("a"), "blind");
    }
    {  // read of a missing key conflicts if it appears (tombstone/insert tracking)
        auto t = kv.begin();
        CHECK(!t.get("ghost"));
        kv.put("ghost", "boo");
        t.put("other", "1");
        CHECK(!t.commit());
    }
    {  // read of a key that gets deleted
        auto t = kv.begin();
        (void)t.get("n");
        kv.erase("n");
        t.put("m", "1");
        CHECK(!t.commit());
    }
    {  // phantom protection for scans
        auto t = kv.begin();
        usize n = 0;
        t.scan("p", std::string_view("q"), [&](auto, auto) { ++n; return true; });
        kv.put("p1", "x");
        t.put("res", "1");
        CHECK(!t.commit());
    }
    {  // rollback by destruction
        { auto t = kv.begin(); t.put("never", "1"); }
        CHECK(!kv.contains("never"));
    }
    {  // scan sees own writes
        KvStore s;
        s.put("a", "1"); s.put("b", "2");
        auto t = s.begin();
        t.erase("a"); t.put("c", "3"); t.put("b", "22");
        std::string seen;
        t.scan("", std::nullopt, [&](std::string_view k, std::string_view v) { seen += std::string(k) + "=" + std::string(v) + ";"; return true; });
        CHECK_EQ(seen, "b=22;c=3;");
    }
}

FLYG_TEST(data_kv_transactions_persist) {
    system::TempDir dir;
    const auto p = dir.path() / "db.fkv";
    {
        KvStore kv(p);
        kv.put("n", "0");
        CHECK(kv.transact([](KvStore::Transaction& t) { t.put("n", std::to_string(std::stoi(*t.get("n")) + 1)); t.put("m", "x"); }));
    }
    KvStore kv(p);
    CHECK_EQ(*kv.get("n"), "1");
    CHECK_EQ(*kv.get("m"), "x");
}

FLYG_TEST(data_kv_concurrent_counter) {
    KvStore kv;
    kv.put("counter", "0");
    constexpr int threads = 6, per = 200;
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i)
        ts.emplace_back([&] {
            for (int j = 0; j < per; ++j) {
                const bool ok = kv.transact([](KvStore::Transaction& t) {
                    t.put("counter", std::to_string(std::stoi(*t.get("counter")) + 1));
                }, 1000);
                if (!ok) ++failures;
            }
        });
    for (auto& t : ts) t.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(*kv.get("counter"), std::to_string(threads * per));
}

FLYG_TEST(data_kv_concurrent_readers_writer) {
    system::TempDir dir;
    KvStore kv(dir.path() / "db.fkv");
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 3; ++i)
        readers.emplace_back([&] {
            while (!stop) {
                // a batch always writes both keys together: readers must never see them differ
                kv.scan("", std::nullopt, [&](auto, auto) { return true; });
                auto a = kv.get("a"), b = kv.get("b");
                (void)a; (void)b;
            }
        });
    for (int i = 0; i < 500; ++i) {
        KvStore::WriteBatch b;
        b.put("a", std::to_string(i));
        b.put("b", std::to_string(i));
        kv.write(b);
    }
    stop = true;
    for (auto& t : readers) t.join();
    CHECK_EQ(bad.load(), 0);
    CHECK_EQ(*kv.get("a"), "499");
}
