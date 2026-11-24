// flyG - document database: collections of Value objects on top of KvStore, with secondary
//        indexes, a Mongo-style query language, a simple planner and transactions
// SPDX-License-Identifier: GPL-2.0-only
//
// Storage layout inside the KvStore (all in one atomic batch per operation/transaction):
//   "C" coll                  -> "1"                 collection marker
//   "M" coll '\0' path        -> "1"|"0"             index definition (unique flag)
//   "S" coll                  -> decimal             next auto-id counter
//   "D" coll '\0' id          -> binary::encode_raw  document
// Indexes live in memory and are rebuilt when the database is opened.
//
// Concurrency: many concurrent readers or one writer. `Database::transaction` makes a group of
// operations atomic (all-or-nothing, also on disk); transactions are flat: an exception anywhere
// rolls back the whole outermost transaction, and nested transaction() calls just join it.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>

#include <flyg/data/binary.hpp>
#include <flyg/data/json.hpp>
#include <flyg/data/kvstore.hpp>
#include <flyg/data/value.hpp>

namespace flyg::data {

struct FindOptions {
    std::vector<std::pair<std::string, int>> sort;  ///< (path, +1 ascending | -1 descending), applied in order
    usize skip = 0;
    usize limit = static_cast<usize>(-1);
    std::vector<std::string> include;  ///< projection: keep only these paths (plus _id unless excluded)
    std::vector<std::string> exclude;  ///< projection: drop these top-level fields
};

class Collection;

class Database {
public:
    /// In-memory database.
    Database() : kv_(std::make_unique<KvStore>()) { load(); }
    explicit Database(const std::filesystem::path& path) : Database(path, KvStore::Options()) {}
    Database(const std::filesystem::path& path, const KvStore::Options& opt) : kv_(std::make_unique<KvStore>(path, opt)) { load(); }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] Collection collection(std::string name);
    [[nodiscard]] std::vector<std::string> collection_names() const;
    /// Removes a collection with all its documents and indexes. Returns false if it did not exist.
    bool drop_collection(const std::string& name);

    /// Runs `fn` atomically. Exceptions roll everything back and propagate. Re-entrant.
    template <class F>
    auto transaction(F&& fn) -> decltype(fn()) {
        return mutate(std::forward<F>(fn));
    }

    [[nodiscard]] KvStore& store() noexcept { return *kv_; }
    void compact() { kv_->compact(); }

private:
    friend class Collection;

    struct VCmp {
        bool operator()(const Value& a, const Value& b) const { return Value::compare(a, b) < 0; }
    };
    struct Index {
        bool unique = false;
        std::multimap<Value, std::string, VCmp> map;
    };
    struct CollState {
        std::map<std::string, Index> idx;
    };
    struct Undo {
        std::string coll, id;
        std::optional<Value> before, after;
    };
    struct Txn {
        std::map<std::string, std::optional<std::string>> overlay;
        std::vector<Undo> undo;
        std::set<std::string> rebuild;  ///< collections whose schema changed: reloaded from the store on rollback
    };

    // ---- key helpers ---------------------------------------------------------------------------
    static void check_name(const std::string& n) {
        if (n.empty() || n.find('\0') != std::string::npos) throw DataException("invalid collection name");
    }
    static std::string doc_prefix(const std::string& c) { return "D" + c + std::string(1, '\0'); }
    static std::string doc_key(const std::string& c, const std::string& id) { return doc_prefix(c) + id; }
    static std::string meta_prefix(const std::string& c) { return "M" + c + std::string(1, '\0'); }

    [[nodiscard]] static std::string encode(const Value& v) {
        ByteWriter w;
        binary::encode_raw(w, v);
        const auto b = w.view();
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    }
    [[nodiscard]] static Value decode(std::string_view s) {
        ByteReader r(std::as_bytes(std::span(s.data(), s.size())));
        return binary::decode_raw(r);
    }

    // ---- locking / transactions ----------------------------------------------------------------
    [[nodiscard]] bool owns() const noexcept { return owner_.load() == std::this_thread::get_id(); }

    struct ReadGuard {
        explicit ReadGuard(const Database& d) {
            if (!d.owns()) lock = std::shared_lock(d.m_);
        }
        std::shared_lock<std::shared_mutex> lock;
    };

    template <class F>
    auto mutate(F&& fn) -> decltype(fn()) {
        if (owns()) return fn();
        std::unique_lock lock(m_);
        owner_.store(std::this_thread::get_id());
        txn_ = Txn{};
        struct Reset {
            Database& d;
            ~Reset() { d.owner_.store(std::thread::id{}); }
        } reset{*this};
        try {
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                commit();
            } else {
                auto r = fn();
                commit();
                return r;
            }
        } catch (...) {
            rollback();
            throw;
        }
    }

    void commit() {
        KvStore::WriteBatch b;
        for (auto& [k, v] : txn_.overlay) {
            if (v) b.put(k, *v);
            else b.erase(k);
        }
        kv_->write(b);  // throws before anything is applied; mutate() then rolls the indexes back
        txn_ = Txn{};
    }
    void rollback() noexcept {
        try {
            for (auto it = txn_.undo.rbegin(); it != txn_.undo.rend(); ++it) {
                if (txn_.rebuild.count(it->coll)) continue;
                if (it->after) unindex(it->coll, it->id, *it->after);
                if (it->before) index_add(it->coll, it->id, *it->before);
            }
            const auto names = txn_.rebuild;
            txn_ = Txn{};
            for (const auto& n : names) reload_collection(n);
        } catch (...) {
        }
        txn_ = Txn{};
    }

    void reload_collection(const std::string& name) {
        states_.erase(name);
        if (!kv_->contains("C" + name)) return;
        CollState& st = states_[name];
        const std::string mp = meta_prefix(name);
        kv_->scan_prefix(mp, [&](std::string_view k, std::string_view v) {
            st.idx[std::string(k.substr(mp.size()))].unique = (v == "1");
            return true;
        });
        const std::string dp = doc_prefix(name);
        kv_->scan_prefix(dp, [&](std::string_view k, std::string_view v) {
            const std::string id(k.substr(dp.size()));
            const Value doc = decode(v);
            for (auto& [path, ix] : st.idx) add_keys(ix, id, doc, path);
            return true;
        });
    }

    // ---- raw access honouring the transaction overlay --------------------------------------------
    [[nodiscard]] std::optional<std::string> raw_get(const std::string& key) const {
        if (owns()) {
            if (auto it = txn_.overlay.find(key); it != txn_.overlay.end()) return it->second;
        }
        return kv_->get(key);
    }
    void raw_put(const std::string& key, std::string v) { txn_.overlay[key] = std::move(v); }
    void raw_erase(const std::string& key) { txn_.overlay[key] = std::nullopt; }

    void raw_scan_prefix(const std::string& prefix, const std::function<bool(std::string_view, std::string_view)>& fn) const {
        if (!owns() || txn_.overlay.empty()) {
            kv_->scan_prefix(prefix, fn);
            return;
        }
        std::map<std::string, std::string> merged;
        kv_->scan_prefix(prefix, [&](std::string_view k, std::string_view v) {
            merged.emplace(k, v);
            return true;
        });
        for (const auto& [k, v] : txn_.overlay) {
            if (k.compare(0, prefix.size(), prefix) != 0) continue;
            if (v) merged[k] = *v;
            else merged.erase(k);
        }
        for (const auto& [k, v] : merged)
            if (!fn(k, v)) return;
    }

    // ---- loading ---------------------------------------------------------------------------------
    void load() {
        kv_->scan_prefix("C", [&](std::string_view k, std::string_view) {
            states_[std::string(k.substr(1))];
            return true;
        });
        for (auto& [name, st] : states_) {
            const std::string mp = meta_prefix(name);
            kv_->scan_prefix(mp, [&](std::string_view k, std::string_view v) {
                st.idx[std::string(k.substr(mp.size()))].unique = (v == "1");
                return true;
            });
            kv_->scan_prefix(doc_prefix(name), [&](std::string_view k, std::string_view v) {
                const std::string id(k.substr(doc_prefix(name).size()));
                const Value doc = decode(v);
                for (auto& [path, ix] : st.idx) add_keys(ix, id, doc, path);
                return true;
            });
        }
    }

    // ---- paths & matching ------------------------------------------------------------------------
    static std::vector<std::string> split_path(std::string_view p) {
        std::vector<std::string> out;
        usize i = 0;
        while (true) {
            const usize j = p.find('.', i);
            out.emplace_back(p.substr(i, j == std::string_view::npos ? j : j - i));
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        return out;
    }
    static bool all_digits(const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
    }
    static void collect(const Value& v, const std::vector<std::string>& steps, usize i, std::vector<const Value*>& out) {
        if (i == steps.size()) {
            out.push_back(&v);
            return;
        }
        if (v.is_object()) {
            if (const Value* f = v.as_object().find(steps[i])) collect(*f, steps, i + 1, out);
        } else if (v.is_array()) {
            const auto& a = v.as_array();
            if (all_digits(steps[i]) && steps[i].size() < 10) {
                const usize n = std::stoull(steps[i]);
                if (n < a.size()) collect(a[n], steps, i + 1, out);
            }
            for (const auto& e : a)
                if (e.is_object()) collect(e, steps, i, out);
        }
    }
    static std::vector<const Value*> resolve(const Value& doc, const std::string& path) {
        std::vector<const Value*> out;
        collect(doc, split_path(path), 0, out);
        return out;
    }

    static int type_class(const Value& v) {
        return v.is_number() ? 100 : static_cast<int>(v.type());
    }
    static bool same_class(const Value& a, const Value& b) { return type_class(a) == type_class(b); }

    template <class P>
    static bool any_of_values(const std::vector<const Value*>& c, P&& pred) {
        for (const Value* v : c) {
            if (pred(*v)) return true;
            if (v->is_array())
                for (const auto& e : v->as_array())
                    if (pred(e)) return true;
        }
        return false;
    }

    static bool is_operator_object(const Value& c) {
        if (!c.is_object() || c.as_object().empty()) return false;
        usize ops = 0;
        for (const auto& [k, v] : c.as_object()) {
            (void)v;
            if (!k.empty() && k[0] == '$') ++ops;
        }
        if (ops != 0 && ops != c.as_object().size()) throw DataException("query: cannot mix operators and fields in one condition");
        return ops != 0;
    }

    static bool type_matches(const Value& v, const std::string& t) {
        if (t == "null") return v.is_null();
        if (t == "bool") return v.is_bool();
        if (t == "int") return v.is_integer();
        if (t == "double") return v.is_real();
        if (t == "number") return v.is_number();
        if (t == "string") return v.is_string();
        if (t == "binary") return v.is_binary();
        if (t == "array") return v.is_array();
        if (t == "object") return v.is_object();
        throw DataException("query: unknown $type '" + t + "'");
    }

    static bool match_ops(const std::vector<const Value*>& c, const Value& ops) {
        for (const auto& [op, arg] : ops.as_object()) {
            bool ok = false;
            if (op == "$eq") {
                ok = any_of_values(c, [&](const Value& v) { return Value::compare(v, arg) == 0; });
            } else if (op == "$ne") {
                ok = !any_of_values(c, [&](const Value& v) { return Value::compare(v, arg) == 0; });
            } else if (op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte") {
                ok = any_of_values(c, [&](const Value& v) {
                    if (!same_class(v, arg)) return false;
                    const auto o = Value::compare(v, arg);
                    return op == "$gt" ? o > 0 : op == "$gte" ? o >= 0 : op == "$lt" ? o < 0 : o <= 0;
                });
            } else if (op == "$in" || op == "$nin") {
                if (!arg.is_array()) throw DataException("query: " + op + " needs an array");
                const bool found = any_of_values(c, [&](const Value& v) {
                    for (const auto& x : arg.as_array())
                        if (Value::compare(v, x) == 0) return true;
                    return false;
                });
                ok = (op == "$in") ? found : !found;
            } else if (op == "$exists") {
                ok = arg.bool_or(true) ? !c.empty() : c.empty();
            } else if (op == "$contains") {
                ok = any_of_values(c, [&](const Value& v) {
                    return (v.is_string() && arg.is_string() && v.as_string().find(arg.as_string()) != std::string::npos);
                });
                if (!ok)
                    for (const Value* v : c)
                        if (v->is_array())
                            for (const auto& e : v->as_array())
                                if (Value::compare(e, arg) == 0) ok = true;
            } else if (op == "$prefix" || op == "$suffix") {
                if (!arg.is_string()) throw DataException("query: " + op + " needs a string");
                const auto& s = arg.as_string();
                ok = any_of_values(c, [&](const Value& v) {
                    if (!v.is_string() || v.as_string().size() < s.size()) return false;
                    const auto& t = v.as_string();
                    return op == "$prefix" ? t.compare(0, s.size(), s) == 0 : t.compare(t.size() - s.size(), s.size(), s) == 0;
                });
            } else if (op == "$size") {
                const i64 n = arg.as_int();
                for (const Value* v : c)
                    if (v->is_array() && static_cast<i64>(v->as_array().size()) == n) ok = true;
            } else if (op == "$type") {
                const auto& t = arg.as_string();
                ok = any_of_values(c, [&](const Value& v) { return type_matches(v, t); });
            } else if (op == "$not") {
                if (!is_operator_object(arg)) throw DataException("query: $not needs an operator object");
                ok = !match_ops(c, arg);
            } else {
                throw DataException("query: unknown operator '" + op + "'");
            }
            if (!ok) return false;
        }
        return true;
    }

    static void validate(const Value& filter) {
        if (filter.is_null()) return;
        if (!filter.is_object()) throw DataException("query: filter must be an object");
        for (const auto& [k, cond] : filter.as_object()) {
            if (k == "$and" || k == "$or" || k == "$nor") {
                if (!cond.is_array() || cond.as_array().empty()) throw DataException("query: " + k + " needs a non-empty array");
                for (const auto& f : cond.as_array()) validate(f);
            } else if (!k.empty() && k[0] == '$') {
                throw DataException("query: unknown top-level operator '" + k + "'");
            } else if (is_operator_object(cond)) {
                validate_ops(cond);
            }
        }
    }
    static void validate_ops(const Value& ops) {
        for (const auto& [op, arg] : ops.as_object()) {
            if (op == "$eq" || op == "$ne" || op == "$gt" || op == "$gte" || op == "$lt" || op == "$lte" || op == "$contains") {
            } else if (op == "$in" || op == "$nin") {
                if (!arg.is_array()) throw DataException("query: " + op + " needs an array");
            } else if (op == "$exists") {
                if (!arg.is_bool()) throw DataException("query: $exists needs a boolean");
            } else if (op == "$prefix" || op == "$suffix") {
                if (!arg.is_string()) throw DataException("query: " + op + " needs a string");
            } else if (op == "$size") {
                if (!arg.is_integer()) throw DataException("query: $size needs an integer");
            } else if (op == "$type") {
                if (!arg.is_string()) throw DataException("query: $type needs a string");
                (void)type_matches(Value(), arg.as_string());
            } else if (op == "$not") {
                if (!is_operator_object(arg)) throw DataException("query: $not needs an operator object");
                validate_ops(arg);
            } else {
                throw DataException("query: unknown operator '" + op + "'");
            }
        }
    }

public:
    /// True if `doc` satisfies `filter` (see file header of the tests/README for the language).
    [[nodiscard]] static bool matches(const Value& doc, const Value& filter) {
        if (filter.is_null() || (filter.is_object() && filter.as_object().empty())) return true;
        if (!filter.is_object()) throw DataException("query: filter must be an object");
        for (const auto& [k, cond] : filter.as_object()) {
            if (k == "$and" || k == "$or" || k == "$nor") {
                if (!cond.is_array() || cond.as_array().empty()) throw DataException("query: " + k + " needs a non-empty array");
                if (k == "$and") {
                    for (const auto& f : cond.as_array())
                        if (!matches(doc, f)) return false;
                } else {
                    bool any = false;
                    for (const auto& f : cond.as_array())
                        if (matches(doc, f)) any = true;
                    if (k == "$or" ? !any : any) return false;
                }
            } else if (!k.empty() && k[0] == '$') {
                throw DataException("query: unknown top-level operator '" + k + "'");
            } else {
                const auto c = resolve(doc, k);
                if (is_operator_object(cond)) {
                    if (!match_ops(c, cond)) return false;
                } else if (!any_of_values(c, [&](const Value& v) { return Value::compare(v, cond) == 0; })) {
                    return false;
                }
            }
        }
        return true;
    }

private:
    // ---- index maintenance -------------------------------------------------------------------------
    static void add_keys(Index& ix, const std::string& id, const Value& doc, const std::string& path) {
        for (const Value* v : resolve(doc, path)) {
            if (v->is_array()) {
                for (const auto& e : v->as_array()) ix.map.emplace(e, id);
            } else {
                ix.map.emplace(*v, id);
            }
        }
    }
    static void remove_keys(Index& ix, const std::string& id, const Value& doc, const std::string& path) {
        auto drop = [&](const Value& key) {
            auto [lo, hi] = ix.map.equal_range(key);
            for (auto it = lo; it != hi; ++it)
                if (it->second == id) {
                    ix.map.erase(it);
                    return;
                }
        };
        for (const Value* v : resolve(doc, path)) {
            if (v->is_array()) {
                for (const auto& e : v->as_array()) drop(e);
            } else {
                drop(*v);
            }
        }
    }
    void index_add(const std::string& coll, const std::string& id, const Value& doc) {
        for (auto& [path, ix] : states_[coll].idx) add_keys(ix, id, doc, path);
    }
    void unindex(const std::string& coll, const std::string& id, const Value& doc) {
        for (auto& [path, ix] : states_[coll].idx) remove_keys(ix, id, doc, path);
    }
    void check_unique(const std::string& coll, const std::string& id, const Value& doc) {
        for (auto& [path, ix] : states_[coll].idx) {
            if (!ix.unique) continue;
            for (const Value* v : resolve(doc, path)) {
                auto test = [&](const Value& key) {
                    auto [lo, hi] = ix.map.equal_range(key);
                    for (auto it = lo; it != hi; ++it)
                        if (it->second != id) throw DataException("unique index violation on '" + path + "' in collection '" + coll + "'");
                };
                if (v->is_array()) {
                    for (const auto& e : v->as_array()) test(e);
                } else {
                    test(*v);
                }
            }
        }
    }

    // ---- document primitives (caller is inside mutate()) --------------------------------------------
    [[nodiscard]] std::optional<Value> read_doc(const std::string& coll, const std::string& id) const {
        const auto s = raw_get(doc_key(coll, id));
        if (!s) return std::nullopt;
        return decode(*s);
    }

    void ensure_collection(const std::string& coll) {
        if (states_.find(coll) == states_.end()) {
            states_[coll];
            txn_.rebuild.insert(coll);
            raw_put("C" + coll, "1");
        }
    }

    /// Stores (or removes, when `after` is empty) a document and keeps indexes and undo log in sync.
    void write_doc(const std::string& coll, const std::string& id, std::optional<Value> before, std::optional<Value> after) {
        if (after) check_unique(coll, id, *after);  // entries of this very id are ignored
        if (before) unindex(coll, id, *before);
        if (after) {
            index_add(coll, id, *after);
            raw_put(doc_key(coll, id), encode(*after));
        } else {
            raw_erase(doc_key(coll, id));
        }
        txn_.undo.push_back({coll, id, std::move(before), std::move(after)});
    }

    std::string next_id(const std::string& coll) {
        u64 n = 0;
        if (const auto s = raw_get("S" + coll)) n = std::stoull(*s);
        for (;;) {
            ++n;
            char buf[17];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(n));
            raw_put("S" + coll, std::to_string(n));
            if (!raw_get(doc_key(coll, buf))) return buf;
        }
    }

    // ---- querying -----------------------------------------------------------------------------------
    struct Plan {
        std::string kind = "scan";  // "scan" | "index"
        std::string path, op;
        std::vector<std::string> ids;
    };

    static void conjuncts(const Value& filter, std::vector<std::pair<std::string, const Value*>>& out) {
        if (!filter.is_object()) return;
        for (const auto& [k, cond] : filter.as_object()) {
            if (k == "$and" && cond.is_array()) {
                for (const auto& f : cond.as_array()) conjuncts(f, out);
            } else if (k.empty() || k[0] != '$') {
                out.emplace_back(k, &cond);
            }
        }
    }

    [[nodiscard]] Plan plan(const std::string& coll, const Value& filter) const {
        Plan best;
        const auto sit = states_.find(coll);
        if (sit == states_.end() || sit->second.idx.empty()) return best;
        std::vector<std::pair<std::string, const Value*>> cs;
        conjuncts(filter, cs);
        int best_score = 1000;
        for (const auto& [path, cond] : cs) {
            const auto iit = sit->second.idx.find(path);
            if (iit == sit->second.idx.end()) continue;
            const Index& ix = iit->second;
            std::vector<std::string> ids;
            std::string op;
            int score = 1000;
            auto add_eq = [&](const Value& x) {
                auto [lo, hi] = ix.map.equal_range(x);
                for (auto it = lo; it != hi; ++it) ids.push_back(it->second);
            };
            if (!is_operator_object(*cond)) {
                if (cond->is_array()) continue;
                add_eq(*cond);
                op = "eq";
                score = ix.unique ? 0 : 1;
            } else {
                const Object& ops = cond->as_object();
                if (const Value* e = ops.find("$eq"); e && !e->is_array()) {
                    add_eq(*e);
                    op = "eq";
                    score = ix.unique ? 0 : 1;
                } else if (const Value* in = ops.find("$in"); in && in->is_array()) {
                    for (const auto& x : in->as_array()) {
                        if (x.is_array()) { ids.clear(); op.clear(); break; }
                        add_eq(x);
                    }
                    if (in->is_array() && std::none_of(in->as_array().begin(), in->as_array().end(), [](const Value& x) { return x.is_array(); })) {
                        op = "in";
                        score = 2;
                    }
                } else {
                    const Value *gt = ops.find("$gt"), *gte = ops.find("$gte"), *lt = ops.find("$lt"), *lte = ops.find("$lte");
                    const Value* lower = gt ? gt : gte;
                    const Value* upper = lt ? lt : lte;
                    if (!lower && !upper) continue;
                    if ((lower && lower->is_array()) || (upper && upper->is_array())) continue;
                    const Value& ref = lower ? *lower : *upper;
                    if (lower && upper && !same_class(*lower, *upper)) continue;
                    auto it = lower ? (gt ? ix.map.upper_bound(*lower) : ix.map.lower_bound(*lower)) : ix.map.begin();
                    for (; it != ix.map.end(); ++it) {
                        if (!same_class(it->first, ref)) {
                            if (lower || Value::compare(it->first, ref) > 0) break;
                            continue;  // upper-only: skip lower type classes
                        }
                        if (upper) {
                            const auto o = Value::compare(it->first, *upper);
                            if (lt ? o >= 0 : o > 0) break;
                        }
                        ids.push_back(it->second);
                    }
                    op = "range";
                    score = 3;
                }
            }
            if (op.empty()) continue;
            if (score < best_score || (score == best_score && ids.size() < best.ids.size())) {
                best_score = score;
                best.kind = "index";
                best.path = path;
                best.op = op;
                best.ids = std::move(ids);
            }
        }
        if (best.kind == "index") {
            std::sort(best.ids.begin(), best.ids.end());
            best.ids.erase(std::unique(best.ids.begin(), best.ids.end()), best.ids.end());
        }
        return best;
    }

    void run_query(const std::string& coll, const Value& filter, const std::function<void(const std::string&, Value&&)>& fn) const {
        validate(filter);
        const Plan p = plan(coll, filter);
        if (p.kind == "index") {
            for (const auto& id : p.ids) {
                auto d = read_doc(coll, id);
                if (d && matches(*d, filter)) fn(id, std::move(*d));
            }
            return;
        }
        const std::string prefix = doc_prefix(coll);
        raw_scan_prefix(prefix, [&](std::string_view k, std::string_view v) {
            Value d = decode(v);
            if (matches(d, filter)) fn(std::string(k.substr(prefix.size())), std::move(d));
            return true;
        });
    }

    static Value project(const Value& doc, const FindOptions& o) {
        if (o.include.empty() && o.exclude.empty()) return doc;
        Value out = Value(Object{});
        if (!o.include.empty()) {
            for (const auto& p : o.include)
                if (const Value* v = doc.find_path(p)) out.ensure_path(p) = *v;
            if (std::find(o.exclude.begin(), o.exclude.end(), "_id") == o.exclude.end())
                if (const Value* id = doc.find("_id")) out["_id"] = *id;
        } else {
            out = doc;
        }
        for (const auto& e : o.exclude) out.as_object().erase(e);
        return out;
    }

    std::unique_ptr<KvStore> kv_;
    mutable std::shared_mutex m_;
    std::atomic<std::thread::id> owner_{};
    std::map<std::string, CollState> states_;
    Txn txn_;
};

class Collection {
public:
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    // ---- indexes -------------------------------------------------------------------------------------
    /// Creates (and persists) an index on a dotted path; existing documents are indexed. With
    /// `unique`, creation fails if existing data already violates it.
    void create_index(const std::string& path, bool unique = false) {
        if (path.empty()) throw DataException("empty index path");
        db_->mutate([&] {
            db_->ensure_collection(name_);
            auto& idx = db_->states_[name_].idx;
            if (idx.count(path)) {
                if (idx[path].unique != unique) throw DataException("index '" + path + "' already exists with different options");
                return;
            }
            Database::Index ix;
            ix.unique = unique;
            db_->raw_scan_prefix(Database::doc_prefix(name_), [&](std::string_view k, std::string_view v) {
                const std::string id(k.substr(Database::doc_prefix(name_).size()));
                Database::add_keys(ix, id, Database::decode(v), path);
                return true;
            });
            if (unique) {
                const Value* prev = nullptr;
                const std::string* prev_id = nullptr;
                for (const auto& [key, id] : ix.map) {
                    if (prev && prev_id && *prev_id != id && Value::compare(*prev, key) == 0)
                        throw DataException("cannot create unique index '" + path + "': duplicate values exist");
                    prev = &key;
                    prev_id = &id;
                }
            }
            idx.emplace(path, std::move(ix));
            db_->raw_put(Database::meta_prefix(name_) + path, unique ? "1" : "0");
            db_->txn_.rebuild.insert(name_);
        });
    }
    bool drop_index(const std::string& path) {
        return db_->mutate([&] {
            const auto sit = db_->states_.find(name_);
            if (sit == db_->states_.end() || !sit->second.idx.erase(path)) return false;
            db_->txn_.rebuild.insert(name_);
            db_->raw_erase(Database::meta_prefix(name_) + path);
            return true;
        });
    }
    [[nodiscard]] std::vector<std::string> indexes() const {
        Database::ReadGuard g(*db_);
        std::vector<std::string> out;
        if (auto it = db_->states_.find(name_); it != db_->states_.end())
            for (const auto& [p, ix] : it->second.idx) out.push_back(p);
        return out;
    }

    // ---- writes --------------------------------------------------------------------------------------
    /// Inserts an object; assigns a string "_id" if missing. Returns the id.
    std::string insert(Value doc) {
        if (!doc.is_object()) throw DataException("documents must be objects");
        return db_->mutate([&] {
            db_->ensure_collection(name_);
            std::string id;
            if (const Value* i = doc.find("_id")) {
                if (!i->is_string() || i->as_string().empty()) throw DataException("_id must be a non-empty string");
                id = i->as_string();
                if (db_->read_doc(name_, id)) throw DataException("duplicate _id '" + id + "' in collection '" + name_ + "'");
            } else {
                id = db_->next_id(name_);
                // keep _id first for readability
                Object o;
                o.insert_or_assign("_id", id);
                for (auto& [k, v] : doc.as_object()) o.insert_or_assign(k, v);
                doc = Value(std::move(o));
            }
            db_->write_doc(name_, id, std::nullopt, std::move(doc));
            return id;
        });
    }
    std::vector<std::string> insert_many(const std::vector<Value>& docs) {
        return db_->mutate([&] {
            std::vector<std::string> ids;
            for (const auto& d : docs) ids.push_back(insert(d));
            return ids;
        });
    }
    /// Replaces the whole document (the `_id` is preserved). Returns false if it does not exist.
    bool replace(const std::string& id, Value doc) {
        if (!doc.is_object()) throw DataException("documents must be objects");
        return db_->mutate([&] {
            auto old = db_->read_doc(name_, id);
            if (!old) return false;
            check_id_unchanged(doc, id);
            doc.as_object().erase("_id");
            Object o;
            o.insert_or_assign("_id", id);
            for (auto& [k, v] : doc.as_object()) o.insert_or_assign(k, v);
            db_->write_doc(name_, id, std::move(old), Value(std::move(o)));
            return true;
        });
    }
    /// Applies an RFC 7386 merge patch. Returns false if the document does not exist.
    bool update(const std::string& id, const Value& patch) {
        return db_->mutate([&] {
            auto old = db_->read_doc(name_, id);
            if (!old) return false;
            Value neu = *old;
            json::merge_patch(neu, patch);
            if (!neu.is_object()) throw DataException("patch must produce an object");
            check_id_unchanged(neu, id);
            neu["_id"] = id;
            db_->write_doc(name_, id, std::move(old), std::move(neu));
            return true;
        });
    }
    bool erase(const std::string& id) {
        return db_->mutate([&] {
            auto old = db_->read_doc(name_, id);
            if (!old) return false;
            db_->write_doc(name_, id, std::move(old), std::nullopt);
            return true;
        });
    }
    /// Applies a merge patch to every matching document; returns how many were changed.
    usize update_many(const Value& filter, const Value& patch) {
        return db_->mutate([&] {
            std::vector<std::string> ids;
            db_->run_query(name_, filter, [&](const std::string& id, Value&&) { ids.push_back(id); });
            for (const auto& id : ids) update(id, patch);
            return ids.size();
        });
    }
    usize erase_many(const Value& filter) {
        return db_->mutate([&] {
            std::vector<std::string> ids;
            db_->run_query(name_, filter, [&](const std::string& id, Value&&) { ids.push_back(id); });
            for (const auto& id : ids) erase(id);
            return ids.size();
        });
    }

    // ---- reads ---------------------------------------------------------------------------------------
    [[nodiscard]] std::optional<Value> get(const std::string& id) const {
        Database::ReadGuard g(*db_);
        return db_->read_doc(name_, id);
    }
    [[nodiscard]] std::vector<Value> find(const Value& filter = Value(Object{}), const FindOptions& opt = {}) const {
        Database::ReadGuard g(*db_);
        std::vector<Value> out;
        const bool unsorted_fast = opt.sort.empty();
        db_->run_query(name_, filter, [&](const std::string&, Value&& d) { out.push_back(std::move(d)); });
        if (!unsorted_fast) {
            std::stable_sort(out.begin(), out.end(), [&](const Value& a, const Value& b) {
                for (const auto& [path, dir] : opt.sort) {
                    const auto ra = Database::resolve(a, path), rb = Database::resolve(b, path);
                    const Value null;
                    const auto o = Value::compare(ra.empty() ? null : *ra.front(), rb.empty() ? null : *rb.front());
                    if (o != 0) return dir >= 0 ? o < 0 : o > 0;
                }
                return false;
            });
        }
        if (opt.skip >= out.size()) return {};
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(opt.skip));
        if (out.size() > opt.limit) out.resize(opt.limit);
        for (auto& d : out) d = Database::project(d, opt);
        return out;
    }
    [[nodiscard]] std::optional<Value> find_one(const Value& filter = Value(Object{}), const FindOptions& opt = {}) const {
        FindOptions o = opt;
        o.limit = 1;
        auto r = find(filter, o);
        if (r.empty()) return std::nullopt;
        return std::move(r.front());
    }
    [[nodiscard]] usize count(const Value& filter = Value(Object{})) const {
        Database::ReadGuard g(*db_);
        usize n = 0;
        db_->run_query(name_, filter, [&](const std::string&, Value&&) { ++n; });
        return n;
    }
    /// Describes how `find(filter)` would be executed: {plan: "scan"|"index", index, op, candidates}.
    [[nodiscard]] Value explain(const Value& filter) const {
        Database::ReadGuard g(*db_);
        Database::validate(filter);
        const auto p = db_->plan(name_, filter);
        Value v = Value(Object{});
        v["plan"] = p.kind;
        if (p.kind == "index") {
            v["index"] = p.path;
            v["op"] = p.op;
            v["candidates"] = static_cast<i64>(p.ids.size());
        }
        return v;
    }

private:
    friend class Database;
    Collection(Database* db, std::string name) : db_(db), name_(std::move(name)) {}
    static void check_id_unchanged(const Value& doc, const std::string& id) {
        if (const Value* i = doc.find("_id"); i && !(i->is_string() && i->as_string() == id)) throw DataException("_id cannot be changed");
    }
    Database* db_;
    std::string name_;
};

inline Collection Database::collection(std::string name) {
    check_name(name);
    return Collection(this, std::move(name));
}
inline std::vector<std::string> Database::collection_names() const {
    ReadGuard g(*this);
    std::vector<std::string> out;
    for (const auto& [n, s] : states_) {
        (void)s;
        out.push_back(n);
    }
    return out;
}
inline bool Database::drop_collection(const std::string& name) {
    return mutate([&] {
        if (!states_.count(name)) return false;
        std::vector<std::string> keys;
        for (const std::string& pre : {doc_prefix(name), meta_prefix(name)})
            raw_scan_prefix(pre, [&](std::string_view k, std::string_view) {
                keys.emplace_back(k);
                return true;
            });
        for (const auto& k : keys) raw_erase(k);
        raw_erase("C" + name);
        raw_erase("S" + name);
        states_.erase(name);
        txn_.rebuild.insert(name);
        return true;
    });
}

}  // namespace flyg::data
