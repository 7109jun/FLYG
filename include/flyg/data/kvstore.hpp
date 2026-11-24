// flyG - embedded ordered key-value store: write-ahead log, crash recovery, compaction,
//        atomic batches and optimistic transactions
// SPDX-License-Identifier: GPL-2.0-only
//
// File format ("FKV1"): 8-byte header, then records [u32 payload_len][u32 crc32(payload)][payload].
// A payload is one atomic batch: varint op_count, then ops {u8 kind(1=put,2=erase), key, [value]}.
// Recovery replays records until the first incomplete/corrupt one (a torn tail after a crash) and
// truncates the file there. Compaction rewrites the live data into a fresh file and atomically
// renames it over the old one. One process may open a database for writing at a time (OS lock).
#pragma once

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <flyg/data/binary.hpp>
#include <flyg/system/native.hpp>

#if FLYG_PLATFORM_LINUX
#include <sys/file.h>
#endif

namespace flyg::data {

namespace detail {

/// Append-only file with an exclusive (or shared, when read-only) inter-process lock.
class LogFile {
public:
    LogFile() = default;
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
    ~LogFile() { close(); }

    void open(const std::filesystem::path& p, bool create, bool read_only) {
        close();
        path_ = p;
#if FLYG_PLATFORM_LINUX
        int flags = (read_only ? O_RDONLY : O_RDWR) | O_CLOEXEC;
        if (create && !read_only) flags |= O_CREAT;
        system::detail::Fd fd(::open(p.c_str(), flags, 0644));
        if (!fd.valid()) throw SystemException::from_errno("cannot open database file '" + p.string() + "'");
        if (::flock(fd.get(), (read_only ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) throw DataException("database '" + p.string() + "' is locked by another process");
            throw SystemException::from_errno("cannot lock database file");
        }
        fd_ = std::move(fd);
#else
        DWORD access = GENERIC_READ | (read_only ? 0 : GENERIC_WRITE);
        DWORD share = read_only ? FILE_SHARE_READ : 0;
        HANDLE h = ::CreateFileW(p.c_str(), access, share, nullptr, (create && !read_only) ? OPEN_ALWAYS : OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION)
                throw DataException("database '" + p.string() + "' is locked by another process");
            throw SystemException("cannot open database file '" + p.string() + "'", static_cast<int>(e));
        }
        h_ = system::detail::Handle(h);
#endif
    }

    void close() noexcept {
#if FLYG_PLATFORM_LINUX
        fd_.reset();
#else
        h_.reset();
#endif
    }

    [[nodiscard]] bool is_open() const noexcept {
#if FLYG_PLATFORM_LINUX
        return fd_.valid();
#else
        return h_.valid();
#endif
    }

    [[nodiscard]] u64 size() const {
#if FLYG_PLATFORM_LINUX
        struct stat st {};
        if (::fstat(fd_.get(), &st) != 0) throw SystemException::from_errno("fstat failed");
        return static_cast<u64>(st.st_size);
#else
        LARGE_INTEGER li{};
        if (!::GetFileSizeEx(h_.get(), &li)) throw SystemException("GetFileSizeEx failed", system::detail::last_error());
        return static_cast<u64>(li.QuadPart);
#endif
    }

    [[nodiscard]] std::vector<std::byte> read_all() const {
        std::vector<std::byte> out(static_cast<usize>(size()));
        usize done = 0;
#if FLYG_PLATFORM_LINUX
        while (done < out.size()) {
            const ssize_t n = ::pread(fd_.get(), out.data() + done, out.size() - done, static_cast<off_t>(done));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw SystemException::from_errno("read failed");
            }
            if (n == 0) break;
            done += static_cast<usize>(n);
        }
#else
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(h_.get(), zero, nullptr, FILE_BEGIN)) throw SystemException("seek failed", system::detail::last_error());
        while (done < out.size()) {
            DWORD n = 0;
            if (!::ReadFile(h_.get(), out.data() + done, static_cast<DWORD>(std::min<usize>(out.size() - done, 1u << 24)), &n, nullptr))
                throw SystemException("read failed", system::detail::last_error());
            if (n == 0) break;
            done += n;
        }
#endif
        out.resize(done);
        return out;
    }

    /// Appends at the end of the file (all-or-error; a partial write is reported as an exception).
    void append(std::span<const std::byte> data) {
#if FLYG_PLATFORM_LINUX
        if (::lseek(fd_.get(), 0, SEEK_END) < 0) throw SystemException::from_errno("seek failed");
        usize done = 0;
        while (done < data.size()) {
            const ssize_t n = ::write(fd_.get(), data.data() + done, data.size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw SystemException::from_errno("write to database file failed");
            }
            done += static_cast<usize>(n);
        }
#else
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(h_.get(), zero, nullptr, FILE_END)) throw SystemException("seek failed", system::detail::last_error());
        usize done = 0;
        while (done < data.size()) {
            DWORD n = 0;
            if (!::WriteFile(h_.get(), data.data() + done, static_cast<DWORD>(std::min<usize>(data.size() - done, 1u << 24)), &n, nullptr))
                throw SystemException("write to database file failed", system::detail::last_error());
            done += n;
        }
#endif
    }

    void truncate(u64 new_size) {
#if FLYG_PLATFORM_LINUX
        if (::ftruncate(fd_.get(), static_cast<off_t>(new_size)) != 0) throw SystemException::from_errno("ftruncate failed");
#else
        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(new_size);
        if (!::SetFilePointerEx(h_.get(), li, nullptr, FILE_BEGIN) || !::SetEndOfFile(h_.get()))
            throw SystemException("truncate failed", system::detail::last_error());
#endif
    }

    void sync() {
#if FLYG_PLATFORM_LINUX
        if (::fdatasync(fd_.get()) != 0) throw SystemException::from_errno("fdatasync failed");
#else
        if (!::FlushFileBuffers(h_.get())) throw SystemException("FlushFileBuffers failed", system::detail::last_error());
#endif
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
#if FLYG_PLATFORM_LINUX
    system::detail::Fd fd_;
#else
    system::detail::Handle h_;
#endif
};

}  // namespace detail

class KvStore {
public:
    struct Options {
        bool create_if_missing = true;
        bool read_only = false;
        bool sync_on_commit = false;      ///< fdatasync after every commit (durable against power loss)
        bool auto_compact = true;
        bool strict_recovery = false;     ///< throw instead of truncating a corrupt/torn log tail
        u64 compact_min_bytes = 4ull << 20;  ///< never compact logs smaller than this
        double compact_ratio = 3.0;       ///< compact when log size > ratio * live data size
    };

    struct Stats {
        usize keys = 0;
        u64 live_bytes = 0;      ///< key+value bytes currently stored
        u64 log_bytes = 0;       ///< size of the log file (0 for in-memory stores)
        u64 commits = 0;         ///< batches committed since open
        u64 compactions = 0;
        u64 recovered_records = 0;        ///< batches replayed at open
        u64 discarded_tail_bytes = 0;     ///< bytes dropped from a torn/corrupt tail at open
    };

    /// A group of puts/erases applied atomically (all or nothing, also across crashes).
    class WriteBatch {
    public:
        void put(std::string key, std::string value) { ops_.push_back({true, std::move(key), std::move(value)}); }
        void erase(std::string key) { ops_.push_back({false, std::move(key), {}}); }
        [[nodiscard]] usize size() const noexcept { return ops_.size(); }
        [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }
        void clear() noexcept { ops_.clear(); }

    private:
        friend class KvStore;
        struct Op {
            bool is_put;
            std::string key;
            std::string value;
        };
        std::vector<Op> ops_;
    };

    class Transaction;

    /// In-memory store (no persistence).
    KvStore() = default;

    /// Opens (and creates) a persistent store at `path`.
    explicit KvStore(const std::filesystem::path& path) : KvStore(path, Options()) {}
    KvStore(const std::filesystem::path& path, const Options& opt) : opt_(opt), path_(path), persistent_(true) { open_file(); }

    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;
    ~KvStore() {
        try {
            if (persistent_ && log_.is_open() && !opt_.read_only) log_.sync();
        } catch (...) {
        }
    }

    // ---- reads ---------------------------------------------------------------------------------
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const {
        std::shared_lock lock(m_);
        const auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        return it->second.value;
    }
    [[nodiscard]] bool contains(std::string_view key) const {
        std::shared_lock lock(m_);
        return data_.find(key) != data_.end();
    }
    [[nodiscard]] usize size() const {
        std::shared_lock lock(m_);
        return data_.size();
    }

    /// Visits keys in [lo, hi) in order (hi == nullopt: unbounded above). The callback returns
    /// false to stop. It runs under a read lock and must not modify this store.
    void scan(std::string_view lo, std::optional<std::string_view> hi, const std::function<bool(std::string_view, std::string_view)>& fn,
              bool reverse = false) const {
        std::shared_lock lock(m_);
        auto first = data_.lower_bound(lo);
        auto last = hi ? data_.lower_bound(*hi) : data_.end();
        if (!reverse) {
            for (auto it = first; it != last; ++it)
                if (!fn(it->first, it->second.value)) return;
        } else {
            for (auto it = last; it != first;) {
                --it;
                if (!fn(it->first, it->second.value)) return;
            }
        }
    }

    void scan_prefix(std::string_view prefix, const std::function<bool(std::string_view, std::string_view)>& fn) const {
        scan(prefix, prefix_end(prefix), fn);
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> range(std::string_view lo, std::optional<std::string_view> hi,
                                                                       usize limit = static_cast<usize>(-1)) const {
        std::vector<std::pair<std::string, std::string>> out;
        scan(lo, hi, [&](std::string_view k, std::string_view v) {
            if (out.size() >= limit) return false;
            out.emplace_back(k, v);
            return out.size() < limit;
        });
        return out;
    }

    /// Smallest string greater than every string with the given prefix (nullopt if there is none).
    [[nodiscard]] static std::optional<std::string> prefix_end(std::string_view prefix) {
        std::string e(prefix);
        while (!e.empty()) {
            if (static_cast<unsigned char>(e.back()) != 0xFF) {
                e.back() = static_cast<char>(static_cast<unsigned char>(e.back()) + 1);
                return e;
            }
            e.pop_back();
        }
        return std::nullopt;
    }

    // ---- writes --------------------------------------------------------------------------------
    void put(std::string key, std::string value) {
        WriteBatch b;
        b.put(std::move(key), std::move(value));
        write(b);
    }
    /// Returns true if the key existed.
    bool erase(std::string_view key) {
        std::unique_lock lock(m_);
        if (data_.find(key) == data_.end()) return false;
        WriteBatch b;
        b.erase(std::string(key));
        commit_locked(b, nullptr);
        return true;
    }
    /// Applies the batch atomically.
    void write(const WriteBatch& batch) {
        if (batch.empty()) return;
        std::unique_lock lock(m_);
        commit_locked(batch, nullptr);
    }

    // ---- transactions --------------------------------------------------------------------------
    /// Optimistic transaction: reads are recorded, writes are buffered; commit() succeeds only if
    /// nothing it read has been changed by another commit since begin (serializable for reads;
    /// blind writes never conflict). commit() returns false on conflict; nothing is applied then.
    class Transaction {
    public:
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&& o) noexcept { *this = std::move(o); }
        Transaction& operator=(Transaction&& o) noexcept {
            if (this != &o) {
                finish();
                store_ = std::exchange(o.store_, nullptr);
                start_seq_ = o.start_seq_;
                reads_ = std::move(o.reads_);
                ranges_ = std::move(o.ranges_);
                writes_ = std::move(o.writes_);
            }
            return *this;
        }
        ~Transaction() { finish(); }

        [[nodiscard]] std::optional<std::string> get(std::string_view key) {
            check();
            if (const auto it = writes_.find(key); it != writes_.end()) return it->second;
            reads_.emplace(key);
            return store_->get(key);
        }
        [[nodiscard]] bool contains(std::string_view key) { return get(key).has_value(); }
        void put(std::string key, std::string value) {
            check();
            writes_[std::move(key)] = std::move(value);
        }
        void erase(std::string key) {
            check();
            writes_[std::move(key)] = std::nullopt;
        }
        /// Ordered scan that sees this transaction's own writes; the range counts as read.
        void scan(std::string_view lo, std::optional<std::string_view> hi, const std::function<bool(std::string_view, std::string_view)>& fn) {
            check();
            ranges_.emplace_back(std::string(lo), hi ? std::optional<std::string>(std::string(*hi)) : std::nullopt);
            std::map<std::string, std::string> merged;
            store_->scan(lo, hi, [&](std::string_view k, std::string_view v) {
                merged.emplace(k, v);
                return true;
            });
            for (const auto& [k, v] : writes_) {
                if (k < lo || (hi && k >= *hi)) continue;
                if (v) merged[k] = *v;
                else merged.erase(k);
            }
            for (const auto& [k, v] : merged)
                if (!fn(k, v)) return;
        }
        /// Atomically applies the writes. Returns false if a conflicting commit happened.
        [[nodiscard]] bool commit() {
            check();
            KvStore* s = std::exchange(store_, nullptr);
            WriteBatch b;
            for (auto& [k, v] : writes_) {
                if (v) b.put(k, *v);
                else b.erase(k);
            }
            std::unique_lock lock(s->m_);
            s->active_.erase(s->active_.find(start_seq_));
            if (s->conflicts(*this)) {
                s->prune_tombstones();
                return false;
            }
            if (!b.empty()) s->commit_locked(b, nullptr);
            s->prune_tombstones();
            return true;
        }
        void rollback() noexcept { finish(); }
        [[nodiscard]] bool active() const noexcept { return store_ != nullptr; }

    private:
        friend class KvStore;
        Transaction(KvStore* s, u64 seq) : store_(s), start_seq_(seq) {}
        void check() const {
            if (!store_) throw DataException("transaction is no longer active");
        }
        void finish() noexcept {
            if (!store_) return;
            try {
                std::unique_lock lock(store_->m_);
                store_->active_.erase(store_->active_.find(start_seq_));
                store_->prune_tombstones();
            } catch (...) {
            }
            store_ = nullptr;
        }

        KvStore* store_ = nullptr;
        u64 start_seq_ = 0;
        std::set<std::string, std::less<>> reads_;
        std::vector<std::pair<std::string, std::optional<std::string>>> ranges_;
        std::map<std::string, std::optional<std::string>, std::less<>> writes_;
    };

    [[nodiscard]] Transaction begin() {
        std::unique_lock lock(m_);
        active_.insert(seq_);
        return Transaction(this, seq_);
    }

    /// Runs `fn(Transaction&)` and commits, retrying (up to `max_attempts`) on conflicts. Returns
    /// true on success. Exceptions from `fn` roll the transaction back and propagate.
    template <class F>
    bool transact(F&& fn, int max_attempts = 16) {
        for (int i = 0; i < max_attempts; ++i) {
            Transaction t = begin();
            fn(t);
            if (t.commit()) return true;
        }
        return false;
    }

    // ---- maintenance ---------------------------------------------------------------------------
    void flush() {
        std::unique_lock lock(m_);
        if (persistent_ && !opt_.read_only) log_.sync();
    }

    /// Rewrites the log so it contains only live data.
    void compact() {
        std::unique_lock lock(m_);
        compact_locked();
    }

    [[nodiscard]] Stats stats() const {
        std::shared_lock lock(m_);
        Stats s = stats_;
        s.keys = data_.size();
        s.live_bytes = live_bytes_;
        s.log_bytes = persistent_ && log_.is_open() ? log_.size() : 0;
        return s;
    }
    [[nodiscard]] bool persistent() const noexcept { return persistent_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    struct Entry {
        std::string value;
        u64 seq = 0;
    };

    static constexpr usize header_size = 8;
    static constexpr u32 max_record = 1u << 30;

    [[nodiscard]] static std::vector<std::byte> make_header() {
        ByteWriter w;
        for (char c : std::string_view("FKVLOG")) w.put_u8(static_cast<core::u8>(c));
        w.put_u8(1);
        w.put_u8(0);
        return w.take();
    }

    [[nodiscard]] static std::vector<std::byte> encode_record(const WriteBatch& b) {
        ByteWriter payload;
        payload.put_varint(b.ops_.size());
        for (const auto& op : b.ops_) {
            payload.put_u8(op.is_put ? 1 : 2);
            payload.put_string(op.key);
            if (op.is_put) payload.put_string(op.value);
        }
        if (payload.size() > max_record) throw DataException("batch too large (limit 1 GiB)");
        ByteWriter rec(payload.size() + 8);
        rec.put_u32(static_cast<u32>(payload.size()));
        rec.put_u32(crc32(payload.view()));
        rec.put_raw(payload.view());
        return rec.take();
    }

    void open_file() {
        if (!opt_.read_only && !opt_.create_if_missing && !std::filesystem::exists(path_))
            throw DataException("database '" + path_.string() + "' does not exist");
        if (opt_.read_only && !std::filesystem::exists(path_)) throw DataException("database '" + path_.string() + "' does not exist");
        // a leftover temporary file means a compaction was interrupted before the rename
        if (!opt_.read_only) {
            std::error_code ec;
            std::filesystem::remove(tmp_path(), ec);
        }
        log_.open(path_, opt_.create_if_missing, opt_.read_only);
        const auto bytes = log_.read_all();
        if (bytes.empty()) {
            if (!opt_.read_only) {
                const auto h = make_header();
                log_.append(h);
                if (opt_.sync_on_commit) log_.sync();
            }
            return;
        }
        const auto header = make_header();
        if (bytes.size() < header_size || !std::equal(header.begin(), header.begin() + 7, bytes.begin()))
            throw DataException("'" + path_.string() + "' is not a flyG key-value database");
        if (static_cast<core::u8>(bytes[6]) != 1) throw DataException("unsupported database version " + std::to_string(static_cast<int>(static_cast<core::u8>(bytes[6]))));
        usize pos = header_size;
        while (pos < bytes.size()) {
            if (!replay_record(bytes, pos)) break;
        }
        if (pos < bytes.size()) {
            stats_.discarded_tail_bytes = bytes.size() - pos;
            if (opt_.strict_recovery) throw DataException("database '" + path_.string() + "' has a corrupt or incomplete tail", pos);
            if (!opt_.read_only) {
                log_.truncate(pos);
                if (opt_.sync_on_commit) log_.sync();
            }
        }
    }

    /// Applies the record starting at `pos`; returns false (without changes) if it is damaged.
    bool replay_record(const std::vector<std::byte>& bytes, usize& pos) {
        if (bytes.size() - pos < 8) return false;
        ByteReader hr(std::span<const std::byte>(bytes).subspan(pos, 8));
        const u32 len = hr.read_u32();
        const u32 crc = hr.read_u32();
        if (len > max_record || bytes.size() - pos - 8 < len) return false;
        const auto payload = std::span<const std::byte>(bytes).subspan(pos + 8, len);
        if (crc32(payload) != crc) return false;
        WriteBatch b;
        try {
            ByteReader r(payload);
            const u64 n = r.read_varint();
            if (n > payload.size()) return false;
            for (u64 i = 0; i < n; ++i) {
                const core::u8 kind = r.read_u8();
                std::string k = r.read_string();
                if (kind == 1) b.put(std::move(k), r.read_string());
                else if (kind == 2) b.erase(std::move(k));
                else return false;
            }
            if (!r.at_end()) return false;
        } catch (const DataException&) {
            return false;
        }
        apply(b, 0);
        ++stats_.recovered_records;
        pos += 8 + len;
        return true;
    }

    [[nodiscard]] std::filesystem::path tmp_path() const {
        auto p = path_;
        p += ".compact.tmp";
        return p;
    }

    static constexpr u64 entry_overhead = 16;

    void apply(const WriteBatch& b, u64 seq) {
        for (const auto& op : b.ops_) {
            const auto it = data_.find(op.key);
            if (op.is_put) {
                if (it != data_.end()) {
                    live_bytes_ -= it->second.value.size();
                    live_bytes_ += op.value.size();
                    it->second.value = op.value;
                    it->second.seq = seq;
                } else {
                    data_.emplace(op.key, Entry{op.value, seq});
                    live_bytes_ += op.key.size() + op.value.size() + entry_overhead;
                    tombstones_.erase(op.key);
                }
            } else if (it != data_.end()) {
                live_bytes_ -= it->first.size() + it->second.value.size() + entry_overhead;
                data_.erase(it);
                if (seq > 0 && !active_.empty()) tombstones_[op.key] = seq;
            }
        }
    }

    void commit_locked(const WriteBatch& b, const void*) {
        if (opt_.read_only) throw DataException("database is opened read-only");
        if (persistent_) {
            const auto rec = encode_record(b);
            log_.append(rec);  // throws before memory is touched: the batch is all-or-nothing
            if (opt_.sync_on_commit) log_.sync();
        }
        apply(b, ++seq_);
        ++stats_.commits;
        if (persistent_ && opt_.auto_compact) {
            const u64 log_size = log_.size();
            if (log_size > opt_.compact_min_bytes && static_cast<double>(log_size) > opt_.compact_ratio * static_cast<double>(live_bytes_ + header_size))
                compact_locked();
        }
    }

    void compact_locked() {
        if (!persistent_) return;
        if (opt_.read_only) throw DataException("database is opened read-only");
        {
            detail::LogFile tmp;
            std::filesystem::remove(tmp_path());
            tmp.open(tmp_path(), true, false);
            tmp.truncate(0);
            tmp.append(make_header());
            WriteBatch chunk;
            usize chunk_bytes = 0;
            auto flush_chunk = [&] {
                if (!chunk.empty()) tmp.append(encode_record(chunk));
                chunk.clear();
                chunk_bytes = 0;
            };
            for (const auto& [k, e] : data_) {
                chunk.put(k, e.value);
                chunk_bytes += k.size() + e.value.size();
                if (chunk_bytes > (1u << 20)) flush_chunk();
            }
            flush_chunk();
            tmp.sync();
        }
        log_.close();
        std::filesystem::rename(tmp_path(), path_);
        log_.open(path_, false, false);
        ++stats_.compactions;
    }

    // ---- transaction support (caller holds the unique lock) -------------------------------------
    [[nodiscard]] u64 key_seq(std::string_view k) const {
        u64 s = 0;
        if (const auto it = data_.find(k); it != data_.end()) s = it->second.seq;
        if (const auto it = tombstones_.find(k); it != tombstones_.end()) s = std::max(s, it->second);
        return s;
    }
    [[nodiscard]] bool conflicts(const Transaction& t) const {
        if (seq_ == t.start_seq_) return false;  // nothing committed since begin
        for (const auto& k : t.reads_)
            if (key_seq(k) > t.start_seq_) return true;
        for (const auto& [lo, hi] : t.ranges_) {
            auto first = data_.lower_bound(lo);
            auto last = hi ? data_.lower_bound(*hi) : data_.end();
            for (auto it = first; it != last; ++it)
                if (it->second.seq > t.start_seq_) return true;
            auto tf = tombstones_.lower_bound(lo);
            auto tl = hi ? tombstones_.lower_bound(*hi) : tombstones_.end();
            for (auto it = tf; it != tl; ++it)
                if (it->second > t.start_seq_) return true;
        }
        return false;
    }
    void prune_tombstones() {
        if (active_.empty()) {
            tombstones_.clear();
            return;
        }
        const u64 oldest = *active_.begin();
        for (auto it = tombstones_.begin(); it != tombstones_.end();) {
            if (it->second <= oldest) it = tombstones_.erase(it);
            else ++it;
        }
    }

    mutable std::shared_mutex m_;
    std::map<std::string, Entry, std::less<>> data_;
    std::map<std::string, u64, std::less<>> tombstones_;
    std::multiset<u64> active_;
    u64 seq_ = 0;
    u64 live_bytes_ = 0;
    Stats stats_;
    Options opt_;
    std::filesystem::path path_;
    bool persistent_ = false;
    detail::LogFile log_;
};

}  // namespace flyg::data
