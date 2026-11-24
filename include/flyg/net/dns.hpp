// flyG - net module: DNS wire format (RFC 1035 + EDNS0) and a stub resolver client
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <map>
#include <random>
#include <sstream>
#include <variant>

#include <flyg/net/socket.hpp>

namespace flyg::net::dns {

enum class Type : u16 { A = 1, NS = 2, CNAME = 5, SOA = 6, PTR = 12, MX = 15, TXT = 16, AAAA = 28, SRV = 33, OPT = 41, ANY = 255 };
enum class Rcode : u8 { ok = 0, format_error = 1, server_failure = 2, name_error = 3, not_implemented = 4, refused = 5 };
inline constexpr u16 class_in = 1;

[[nodiscard]] inline std::string_view to_string(Type t) noexcept {
    switch (t) {
        case Type::A: return "A";
        case Type::NS: return "NS";
        case Type::CNAME: return "CNAME";
        case Type::SOA: return "SOA";
        case Type::PTR: return "PTR";
        case Type::MX: return "MX";
        case Type::TXT: return "TXT";
        case Type::AAAA: return "AAAA";
        case Type::SRV: return "SRV";
        case Type::OPT: return "OPT";
        case Type::ANY: return "ANY";
    }
    return "?";
}

struct Question {
    std::string name;
    Type type = Type::A;
    u16 klass = class_in;
    friend bool operator==(const Question&, const Question&) = default;
};

struct Mx {
    u16 preference = 0;
    std::string exchange;
    friend bool operator==(const Mx&, const Mx&) = default;
};
struct Soa {
    std::string mname, rname;
    u32 serial = 0, refresh = 0, retry = 0, expire = 0, minimum = 0;
    friend bool operator==(const Soa&, const Soa&) = default;
};
struct Srv {
    u16 priority = 0, weight = 0, port = 0;
    std::string target;
    friend bool operator==(const Srv&, const Srv&) = default;
};
struct Txt {
    std::vector<std::string> strings;
    friend bool operator==(const Txt&, const Txt&) = default;
};
struct NameData {  // NS, CNAME, PTR
    std::string name;
    friend bool operator==(const NameData&, const NameData&) = default;
};
using Raw = std::vector<u8>;
using RData = std::variant<IpAddress, NameData, Mx, Soa, Srv, Txt, Raw>;

struct Record {
    std::string name;
    Type type = Type::A;
    u16 klass = class_in;
    u32 ttl = 0;
    RData data;
    friend bool operator==(const Record&, const Record&) = default;
};

namespace detail {

[[nodiscard]] inline std::vector<std::string> split_labels(std::string_view name) {
    std::vector<std::string> labels;
    if (name == ".") return labels;  // the root
    std::string cur;
    for (usize i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '\\' && i + 1 < name.size()) {
            if (i + 3 < name.size() && std::isdigit(static_cast<unsigned char>(name[i + 1])) && std::isdigit(static_cast<unsigned char>(name[i + 2])) &&
                std::isdigit(static_cast<unsigned char>(name[i + 3]))) {
                const int v = (name[i + 1] - '0') * 100 + (name[i + 2] - '0') * 10 + (name[i + 3] - '0');
                if (v > 255) throw NetworkException("invalid escape in DNS name");
                cur += static_cast<char>(v);
                i += 3;
            } else {
                cur += name[++i];
            }
        } else if (c == '.') {
            if (cur.empty()) throw NetworkException("empty label in DNS name '" + std::string(name) + "'");
            labels.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) labels.push_back(std::move(cur));
    return labels;
}

[[nodiscard]] inline std::string escape_label(std::string_view l) {
    std::string out;
    for (char ch : l) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '.' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x21 || c > 0x7E) {
            out += '\\';
            out += static_cast<char>('0' + c / 100);
            out += static_cast<char>('0' + (c / 10) % 10);
            out += static_cast<char>('0' + c % 10);
        } else {
            out += ch;
        }
    }
    return out;
}

class Writer {
public:
    std::vector<u8> out;
    std::map<std::string, u16> names;  // lower-cased suffix -> offset

    void u8_(u8 v) { out.push_back(v); }
    void u16_(u16 v) { out.push_back(static_cast<u8>(v >> 8)), out.push_back(static_cast<u8>(v)); }
    void u32_(u32 v) { u16_(static_cast<u16>(v >> 16)); u16_(static_cast<u16>(v)); }

    void name(std::string_view n, bool compress = true) {
        const auto labels = split_labels(n);
        usize total = 1;
        for (const auto& l : labels) {
            if (l.size() > 63) throw NetworkException("DNS label longer than 63 bytes");
            total += l.size() + 1;
        }
        if (total > 255) throw NetworkException("DNS name longer than 255 bytes");
        for (usize i = 0; i < labels.size(); ++i) {
            std::string suffix;
            for (usize j = i; j < labels.size(); ++j) {
                for (char c : labels[j]) suffix += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
                suffix += '.';
            }
            if (compress) {
                if (auto it = names.find(suffix); it != names.end()) {
                    u16_(static_cast<u16>(0xC000 | it->second));
                    return;
                }
            }
            if (out.size() < 0x3FFF) names.emplace(suffix, static_cast<u16>(out.size()));
            u8_(static_cast<u8>(labels[i].size()));
            out.insert(out.end(), labels[i].begin(), labels[i].end());
        }
        u8_(0);
    }
};

class Reader {
public:
    Reader(const u8* d, usize n) : d_(d), n_(n) {}
    usize pos = 0;
    [[nodiscard]] usize size() const noexcept { return n_; }
    [[nodiscard]] const u8* data() const noexcept { return d_; }
    [[noreturn]] static void fail(const char* w) { throw NetworkException(std::string("malformed DNS message: ") + w); }
    u8 r8() {
        if (pos + 1 > n_) fail("truncated");
        return d_[pos++];
    }
    u16 r16() {
        if (pos + 2 > n_) fail("truncated");
        const u16 v = static_cast<u16>((d_[pos] << 8) | d_[pos + 1]);
        pos += 2;
        return v;
    }
    u32 r32() {
        const u32 hi = r16();
        return (hi << 16) | r16();
    }
    /// Reads a (possibly compressed) name starting at `pos`.
    std::string name() {
        std::string out;
        usize p = pos;
        bool jumped = false;
        int hops = 0;
        usize total = 0;
        for (;;) {
            if (p >= n_) fail("name runs past the message");
            const u8 len = d_[p];
            if ((len & 0xC0) == 0xC0) {
                if (p + 2 > n_) fail("truncated compression pointer");
                const usize target = static_cast<usize>(((len & 0x3F) << 8) | d_[p + 1]);
                if (!jumped) pos = p + 2;
                jumped = true;
                if (target >= p || ++hops > 64) fail("bad compression pointer");  // must point backwards
                p = target;
                continue;
            }
            if (len & 0xC0) fail("unsupported label type");
            ++p;
            if (len == 0) break;
            if (p + len > n_) fail("label runs past the message");
            total += len + 1;
            if (total > 255) fail("name too long");
            if (!out.empty()) out += '.';
            out += escape_label(std::string_view(reinterpret_cast<const char*>(d_ + p), len));
            p += len;
        }
        if (!jumped) pos = p;
        return out.empty() ? std::string(".") : out;
    }

private:
    const u8* d_;
    usize n_;
};

}  // namespace detail

struct Message {
    u16 id = 0;
    bool response = false;
    u8 opcode = 0;
    bool authoritative = false;
    bool truncated = false;
    bool recursion_desired = true;
    bool recursion_available = false;
    Rcode rcode = Rcode::ok;
    std::vector<Question> questions;
    std::vector<Record> answers, authority, additional;
    u16 edns_udp_size = 0;  ///< non-zero: an EDNS0 OPT record with this payload size is sent/was received

    [[nodiscard]] static Message query(std::string name, Type type, u16 id, bool edns = true) {
        Message m;
        m.id = id;
        m.questions.push_back({std::move(name), type, class_in});
        if (edns) m.edns_udp_size = 1232;
        return m;
    }

    [[nodiscard]] std::vector<u8> encode() const {
        detail::Writer w;
        w.u16_(id);
        u16 flags = static_cast<u16>((response ? 0x8000 : 0) | ((opcode & 15) << 11) | (authoritative ? 0x0400 : 0) | (truncated ? 0x0200 : 0) |
                                     (recursion_desired ? 0x0100 : 0) | (recursion_available ? 0x0080 : 0) | (static_cast<u8>(rcode) & 15));
        w.u16_(flags);
        w.u16_(static_cast<u16>(questions.size()));
        w.u16_(static_cast<u16>(answers.size()));
        w.u16_(static_cast<u16>(authority.size()));
        w.u16_(static_cast<u16>(additional.size() + (edns_udp_size ? 1 : 0)));
        for (const auto& q : questions) {
            w.name(q.name);
            w.u16_(static_cast<u16>(q.type));
            w.u16_(q.klass);
        }
        for (const auto* sec : {&answers, &authority, &additional})
            for (const auto& r : *sec) write_record(w, r);
        if (edns_udp_size) {
            w.u8_(0);  // root name
            w.u16_(static_cast<u16>(Type::OPT));
            w.u16_(edns_udp_size);
            w.u32_(0);
            w.u16_(0);
        }
        return std::move(w.out);
    }

    [[nodiscard]] static Message decode(std::span<const std::byte> bytes) {
        detail::Reader r(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
        if (bytes.size() < 12) detail::Reader::fail("shorter than a header");
        Message m;
        m.id = r.r16();
        const u16 f = r.r16();
        m.response = f & 0x8000;
        m.opcode = static_cast<u8>((f >> 11) & 15);
        m.authoritative = f & 0x0400;
        m.truncated = f & 0x0200;
        m.recursion_desired = f & 0x0100;
        m.recursion_available = f & 0x0080;
        m.rcode = static_cast<Rcode>(f & 15);
        const u16 qd = r.r16(), an = r.r16(), ns = r.r16(), ar = r.r16();
        // every entry needs at least a few bytes, so counts larger than the packet are bogus
        if (static_cast<usize>(qd) * 5 + static_cast<usize>(an + ns + ar) * 11 > bytes.size()) detail::Reader::fail("section counts exceed the message size");
        for (u16 i = 0; i < qd; ++i) {
            Question q;
            q.name = r.name();
            q.type = static_cast<Type>(r.r16());
            q.klass = r.r16();
            m.questions.push_back(std::move(q));
        }
        auto section = [&](std::vector<Record>& out, u16 n, bool additional_section) {
            for (u16 i = 0; i < n; ++i) {
                Record rec = read_record(r);
                if (rec.type == Type::OPT && additional_section) {
                    m.edns_udp_size = rec.klass;
                    continue;
                }
                out.push_back(std::move(rec));
            }
        };
        section(m.answers, an, false);
        section(m.authority, ns, false);
        section(m.additional, ar, true);
        return m;
    }

private:
    static void write_record(detail::Writer& w, const Record& r) {
        w.name(r.name);
        w.u16_(static_cast<u16>(r.type));
        w.u16_(r.klass);
        w.u32_(r.ttl);
        const usize len_at = w.out.size();
        w.u16_(0);
        const usize start = w.out.size();
        std::visit(
            [&](const auto& d) {
                using D = std::decay_t<decltype(d)>;
                if constexpr (std::is_same_v<D, IpAddress>) {
                    w.out.insert(w.out.end(), d.data(), d.data() + d.size());
                } else if constexpr (std::is_same_v<D, NameData>) {
                    w.name(d.name);
                } else if constexpr (std::is_same_v<D, Mx>) {
                    w.u16_(d.preference);
                    w.name(d.exchange);
                } else if constexpr (std::is_same_v<D, Soa>) {
                    w.name(d.mname);
                    w.name(d.rname);
                    w.u32_(d.serial), w.u32_(d.refresh), w.u32_(d.retry), w.u32_(d.expire), w.u32_(d.minimum);
                } else if constexpr (std::is_same_v<D, Srv>) {
                    w.u16_(d.priority), w.u16_(d.weight), w.u16_(d.port);
                    w.name(d.target, false);
                } else if constexpr (std::is_same_v<D, Txt>) {
                    for (const auto& s : d.strings) {
                        if (s.size() > 255) throw NetworkException("TXT string longer than 255 bytes");
                        w.u8_(static_cast<u8>(s.size()));
                        w.out.insert(w.out.end(), s.begin(), s.end());
                    }
                } else {
                    w.out.insert(w.out.end(), d.begin(), d.end());
                }
            },
            r.data);
        const usize len = w.out.size() - start;
        if (len > 0xFFFF) throw NetworkException("DNS record too large");
        w.out[len_at] = static_cast<u8>(len >> 8);
        w.out[len_at + 1] = static_cast<u8>(len);
    }

    static Record read_record(detail::Reader& r) {
        Record rec;
        rec.name = r.name();
        rec.type = static_cast<Type>(r.r16());
        rec.klass = r.r16();
        rec.ttl = r.r32();
        const u16 rdlen = r.r16();
        if (r.pos + rdlen > r.size()) detail::Reader::fail("record data runs past the message");
        const usize end = r.pos + rdlen;
        switch (rec.type) {
            case Type::A:
                if (rdlen != 4) detail::Reader::fail("bad A record length");
                rec.data = IpAddress::v4(r.data()[r.pos], r.data()[r.pos + 1], r.data()[r.pos + 2], r.data()[r.pos + 3]);
                r.pos = end;
                break;
            case Type::AAAA: {
                if (rdlen != 16) detail::Reader::fail("bad AAAA record length");
                std::array<u8, 16> b{};
                std::memcpy(b.data(), r.data() + r.pos, 16);
                rec.data = IpAddress::v6(b);
                r.pos = end;
                break;
            }
            case Type::NS:
            case Type::CNAME:
            case Type::PTR: rec.data = NameData{r.name()}; break;
            case Type::MX: {
                Mx mx;
                mx.preference = r.r16();
                mx.exchange = r.name();
                rec.data = std::move(mx);
                break;
            }
            case Type::SOA: {
                Soa s;
                s.mname = r.name();
                s.rname = r.name();
                s.serial = r.r32(), s.refresh = r.r32(), s.retry = r.r32(), s.expire = r.r32(), s.minimum = r.r32();
                rec.data = std::move(s);
                break;
            }
            case Type::SRV: {
                Srv s;
                s.priority = r.r16(), s.weight = r.r16(), s.port = r.r16();
                s.target = r.name();
                rec.data = std::move(s);
                break;
            }
            case Type::TXT: {
                Txt t;
                while (r.pos < end) {
                    const u8 l = r.r8();
                    if (r.pos + l > end) detail::Reader::fail("TXT string runs past the record");
                    t.strings.emplace_back(reinterpret_cast<const char*>(r.data() + r.pos), l);
                    r.pos += l;
                }
                rec.data = std::move(t);
                break;
            }
            default: {
                rec.data = Raw(r.data() + r.pos, r.data() + end);
                r.pos = end;
            }
        }
        if (r.pos != end) detail::Reader::fail("record data length mismatch");
        return rec;
    }
};

// ---- system configuration ---------------------------------------------------------------------------

/// DNS servers configured on this machine (resolv.conf / Windows network parameters).
[[nodiscard]] inline std::vector<Endpoint> system_servers() {
    std::vector<Endpoint> out;
#if FLYG_PLATFORM_WINDOWS
    ULONG size = 0;
    ::GetNetworkParams(nullptr, &size);
    std::vector<unsigned char> buf(size ? size : 1024);
    auto* fi = reinterpret_cast<FIXED_INFO*>(buf.data());
    if (::GetNetworkParams(fi, &size) == NO_ERROR) {
        for (const IP_ADDR_STRING* p = &fi->DnsServerList; p; p = p->Next)
            if (auto a = IpAddress::parse(p->IpAddress.String)) out.emplace_back(*a, 53);
    }
#else
    std::ifstream f("/etc/resolv.conf");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string key, val;
        ss >> key >> val;
        if (key == "nameserver") {
            if (auto a = IpAddress::parse(val)) out.emplace_back(*a, 53);
        }
    }
#endif
    return out;
}

/// Looks `name` up in the hosts file (case-insensitive). Returns all matching addresses.
[[nodiscard]] inline std::vector<IpAddress> hosts_lookup(std::string_view name, std::string path = {}) {
    if (path.empty()) {
#if FLYG_PLATFORM_WINDOWS
        char win[MAX_PATH] = {};
        ::GetSystemDirectoryA(win, MAX_PATH);
        path = std::string(win) + "\\drivers\\etc\\hosts";
#else
        path = "/etc/hosts";
#endif
    }
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        return s;
    };
    const std::string want = lower(std::string(name));
    std::vector<IpAddress> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (const auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::string addr, host;
        if (!(ss >> addr)) continue;
        const auto ip = IpAddress::parse(addr);
        if (!ip) continue;
        while (ss >> host)
            if (lower(host) == want) out.push_back(*ip);
    }
    return out;
}

// ---- resolver ------------------------------------------------------------------------------------------

struct ResolverOptions {
    std::vector<Endpoint> servers;      ///< empty: system_servers()
    Timeout timeout{2000};              ///< per attempt
    int attempts = 2;                   ///< per server
    bool use_edns = true;
    bool use_tcp_on_truncation = true;
    bool consult_hosts_file = true;
};

class Resolver {
public:
    Resolver() : Resolver(ResolverOptions{}) {}
    explicit Resolver(ResolverOptions opt) : opt_(std::move(opt)) {
        if (opt_.servers.empty()) opt_.servers = system_servers();
    }
    [[nodiscard]] const ResolverOptions& options() const noexcept { return opt_; }

    /// Sends a query and returns the validated response. Tries every server in order.
    /// Throws NetworkException if no server answers or all answer with a failure.
    [[nodiscard]] Message query(const std::string& name, Type type) const {
        if (opt_.servers.empty()) throw NetworkException("no DNS servers configured");
        std::string last_error = "no response";
        for (const auto& server : opt_.servers) {
            for (int attempt = 0; attempt < opt_.attempts; ++attempt) {
                try {
                    Message m = exchange(server, name, type);
                    if (m.rcode == Rcode::server_failure || m.rcode == Rcode::refused || m.rcode == Rcode::not_implemented) {
                        last_error = "server " + server.to_string() + " answered rcode " + std::to_string(static_cast<int>(m.rcode));
                        break;  // try the next server
                    }
                    return m;
                } catch (const TimeoutException&) {
                    last_error = "timeout from " + server.to_string();
                } catch (const NetworkException& e) {
                    last_error = e.what();
                }
            }
        }
        throw NetworkException("DNS query for '" + name + "' failed: " + last_error);
    }

    /// Address lookup (A and/or AAAA), following CNAME chains returned by the server.
    /// Throws if the name does not exist; returns an empty list if it exists without addresses.
    [[nodiscard]] std::vector<IpAddress> addresses(const std::string& name, Family family = Family::any) const {
        if (const auto lit = IpAddress::parse(name)) return {*lit};
        std::vector<IpAddress> out;
        if (opt_.consult_hosts_file) {
            for (const auto& a : hosts_lookup(name))
                if (family == Family::any || a.family() == family) out.push_back(a);
            if (!out.empty()) return out;
        }
        bool exists = false;
        std::string last_error;
        for (Type t : {Type::A, Type::AAAA}) {
            if ((t == Type::A && family == Family::v6) || (t == Type::AAAA && family == Family::v4)) continue;
            try {
                const Message m = query(name, t);
                if (m.rcode == Rcode::name_error) continue;
                exists = true;
                for (const auto& a : follow_cname(m, name, t))
                    if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
            } catch (const NetworkException& e) {
                last_error = e.what();
            }
        }
        if (!exists && out.empty()) throw NetworkException(last_error.empty() ? "DNS name '" + name + "' does not exist" : last_error);
        return out;
    }

    [[nodiscard]] std::vector<Mx> mx(const std::string& name) const { return collect<Mx>(query(name, Type::MX), Type::MX); }
    [[nodiscard]] std::vector<Txt> txt(const std::string& name) const { return collect<Txt>(query(name, Type::TXT), Type::TXT); }
    [[nodiscard]] std::vector<Srv> srv(const std::string& name) const { return collect<Srv>(query(name, Type::SRV), Type::SRV); }
    /// PTR lookup; returns host names.
    [[nodiscard]] std::vector<std::string> reverse(const IpAddress& ip) const {
        std::vector<std::string> out;
        for (const auto& n : collect<NameData>(query(ip.reverse_name(), Type::PTR), Type::PTR)) out.push_back(n.name);
        return out;
    }

private:
    template <class T>
    static std::vector<T> collect(const Message& m, Type t) {
        std::vector<T> out;
        for (const auto& r : m.answers)
            if (r.type == t)
                if (const T* v = std::get_if<T>(&r.data)) out.push_back(*v);
        return out;
    }

    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    }

    static std::vector<IpAddress> follow_cname(const Message& m, const std::string& name, Type t) {
        std::vector<IpAddress> out;
        std::string cur = lower(name);
        for (int hop = 0; hop < 16; ++hop) {
            bool advanced = false;
            for (const auto& r : m.answers) {
                if (lower(r.name) != cur) continue;
                if (r.type == t) {
                    if (const auto* ip = std::get_if<IpAddress>(&r.data)) out.push_back(*ip);
                } else if (r.type == Type::CNAME && !advanced) {
                    if (const auto* n = std::get_if<NameData>(&r.data)) {
                        cur = lower(n->name);
                        advanced = true;
                    }
                }
            }
            if (!advanced) break;
        }
        return out;
    }

    static u16 random_id() {
        static std::mutex m;
        static std::mt19937 gen{std::random_device{}()};
        std::lock_guard lock(m);
        return static_cast<u16>(gen());
    }

    [[nodiscard]] Message exchange(const Endpoint& server, const std::string& name, Type type) const {
        const u16 id = random_id();
        const Message q = Message::query(name, type, id, opt_.use_edns);
        const auto wire = q.encode();
        auto valid = [&](const Message& m) { return m.response && m.id == id && m.questions.size() == 1 && m.questions[0].type == type; };

        auto sock = UdpSocket::open(server.address().family());
        sock.connect(server);  // the kernel then drops datagrams from other sources
        sock.send(std::as_bytes(std::span(wire)));
        std::vector<std::byte> buf(4096);
        const auto deadline = std::chrono::steady_clock::now() + opt_.timeout;
        for (;;) {
            auto left = std::chrono::duration_cast<Timeout>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) throw TimeoutException("DNS query to " + server.to_string());
            const auto d = sock.receive_for(buf, left);
            if (!d) throw TimeoutException("DNS query to " + server.to_string());
            Message m;
            try {
                m = Message::decode(std::span<const std::byte>(buf.data(), d->size));
            } catch (const NetworkException&) {
                continue;  // garbage: keep waiting for the real answer
            }
            if (!valid(m)) continue;  // wrong id or not an answer to our question: ignore (spoofing resistance)
            if (m.truncated && opt_.use_tcp_on_truncation) return exchange_tcp(server, wire, id, type);
            return m;
        }
    }

    [[nodiscard]] Message exchange_tcp(const Endpoint& server, const std::vector<u8>& wire, u16 id, Type type) const {
        auto s = TcpStream::connect(server, opt_.timeout);
        s.set_read_timeout(opt_.timeout);
        s.set_write_timeout(opt_.timeout);
        std::vector<std::byte> framed(wire.size() + 2);
        framed[0] = static_cast<std::byte>(wire.size() >> 8);
        framed[1] = static_cast<std::byte>(wire.size());
        std::memcpy(framed.data() + 2, wire.data(), wire.size());
        s.write_all(framed);
        std::byte hdr[2];
        s.read_exact(hdr);
        const usize len = (static_cast<usize>(hdr[0]) << 8) | static_cast<usize>(hdr[1]);
        std::vector<std::byte> body(len);
        s.read_exact(body);
        Message m = Message::decode(body);
        if (!m.response || m.id != id || m.questions.size() != 1 || m.questions[0].type != type) throw NetworkException("unexpected DNS response over TCP");
        return m;
    }

    ResolverOptions opt_;
};

}  // namespace flyg::net::dns
