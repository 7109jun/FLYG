// flyG - graphics module: triangle meshes, procedural primitives, Wavefront OBJ/MTL loader
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <flyg/graphics/color.hpp>
#include <flyg/graphics/path.hpp>
#include <flyg/math/matrix.hpp>
#include <flyg/math/vector2.hpp>
#include <flyg/math/vector3.hpp>
#include <flyg/math/vector4.hpp>

namespace flyg::graphics {

using math::Matrix4f;
using math::Vector2f;
using math::Vector3f;
using math::Vector4f;

struct Vertex {
    Vector3f position;
    Vector3f normal{0, 1, 0};
    Vector2f uv;
    Vector4f color{1, 1, 1, 1};
};

struct Material {
    std::string name;
    Vector3f ambient{0.2f, 0.2f, 0.2f}, diffuse{0.8f, 0.8f, 0.8f}, specular{0, 0, 0}, emission{0, 0, 0};
    float shininess = 32, opacity = 1;
    std::string diffuse_map, normal_map;
};

struct MeshGroup {
    std::string name, material;
    usize first_index = 0, index_count = 0;
};

struct Bounds3 {
    Vector3f min{0, 0, 0}, max{0, 0, 0};
    [[nodiscard]] Vector3f center() const { return (min + max) * 0.5f; }
    [[nodiscard]] Vector3f size() const { return max - min; }
};

/// Indexed triangle list; counter-clockwise triangles face outward.
class Mesh {
public:
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::vector<MeshGroup> groups;  ///< optional sub-ranges of `indices` (from OBJ groups / usemtl)

    [[nodiscard]] usize triangle_count() const noexcept { return indices.size() / 3; }
    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }

    /// Throws if an index points outside the vertex array or the index count is not a multiple of 3.
    void validate() const {
        if (indices.size() % 3) throw GraphicsException("mesh index count is not a multiple of 3");
        for (u32 i : indices)
            if (i >= vertices.size()) throw GraphicsException("mesh index out of range");
    }
    [[nodiscard]] Bounds3 bounds() const {
        if (vertices.empty()) return {};
        Bounds3 b{vertices[0].position, vertices[0].position};
        for (const Vertex& v : vertices) {
            b.min = {std::min(b.min.x, v.position.x), std::min(b.min.y, v.position.y), std::min(b.min.z, v.position.z)};
            b.max = {std::max(b.max.x, v.position.x), std::max(b.max.y, v.position.y), std::max(b.max.z, v.position.z)};
        }
        return b;
    }
    /// Area-weighted smooth vertex normals.
    void compute_normals() {
        validate();
        std::vector<Vector3f> acc(vertices.size(), Vector3f{0, 0, 0});
        for (usize t = 0; t + 2 < indices.size(); t += 3) {
            const Vector3f& a = vertices[indices[t]].position;
            const Vector3f& b = vertices[indices[t + 1]].position;
            const Vector3f& c = vertices[indices[t + 2]].position;
            const Vector3f n = math::cross(b - a, c - a);  // length = 2*area
            for (usize k = 0; k < 3; ++k) acc[indices[t + k]] += n;
        }
        for (usize i = 0; i < vertices.size(); ++i) {
            const float l = math::length(acc[i]);
            vertices[i].normal = l > 1e-20f ? acc[i] * (1.0f / l) : Vector3f{0, 1, 0};
        }
    }
    /// Duplicate vertices per triangle and use face normals (faceted look).
    void make_flat() {
        validate();
        std::vector<Vertex> nv;
        nv.reserve(indices.size());
        std::vector<u32> ni(indices.size());
        for (usize t = 0; t + 2 < indices.size(); t += 3) {
            Vertex a = vertices[indices[t]], b = vertices[indices[t + 1]], c = vertices[indices[t + 2]];
            Vector3f n = math::cross(b.position - a.position, c.position - a.position);
            const float l = math::length(n);
            n = l > 1e-20f ? n * (1.0f / l) : Vector3f{0, 1, 0};
            a.normal = b.normal = c.normal = n;
            for (usize k = 0; k < 3; ++k) ni[t + k] = static_cast<u32>(nv.size() + k);
            nv.push_back(a), nv.push_back(b), nv.push_back(c);
        }
        vertices = std::move(nv);
        indices = std::move(ni);
    }
    void flip_winding() {
        for (usize t = 0; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
        for (Vertex& v : vertices) v.normal = v.normal * -1.0f;
    }
    /// Apply an affine transform (normals use the inverse transpose of the upper 3x3).
    void transform(const Matrix4f& m) {
        const Matrix4f nm = math::transpose(math::inverse(m));
        for (Vertex& v : vertices) {
            v.position = math::transform_point(m, v.position);
            const Vector3f n = math::transform_direction(nm, v.normal);
            const float l = math::length(n);
            v.normal = l > 1e-20f ? n * (1.0f / l) : v.normal;
        }
        if (math::determinant(m) < 0) {  // mirrored: keep triangles facing outward
            for (usize t = 0; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
        }
    }
    void append(const Mesh& o) {
        const u32 base = static_cast<u32>(vertices.size());
        const usize ib = indices.size();
        vertices.insert(vertices.end(), o.vertices.begin(), o.vertices.end());
        for (u32 i : o.indices) indices.push_back(i + base);
        for (MeshGroup g : o.groups) { g.first_index += ib; groups.push_back(g); }
    }
    /// Move to the origin and scale so the largest dimension is `size`.
    void normalise_size(float size = 1.0f) {
        const Bounds3 b = bounds();
        const Vector3f s = b.size();
        const float m = std::max({s.x, s.y, s.z});
        if (m <= 0) return;
        const Vector3f c = b.center();
        for (Vertex& v : vertices) v.position = (v.position - c) * (size / m);
    }
};

namespace primitives {

namespace detail {
inline void quad(Mesh& m, Vertex a, Vertex b, Vertex c, Vertex d) {  // CCW a,b,c,d
    const u32 i = static_cast<u32>(m.vertices.size());
    m.vertices.push_back(a), m.vertices.push_back(b), m.vertices.push_back(c), m.vertices.push_back(d);
    for (u32 k : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(i + k);
}
}  // namespace detail

/// Axis-aligned box centred at the origin.
[[nodiscard]] inline Mesh box(float sx = 1, float sy = 1, float sz = 1) {
    Mesh m;
    const float x = sx / 2, y = sy / 2, z = sz / 2;
    auto face = [&](Vector3f n, Vector3f p0, Vector3f p1, Vector3f p2, Vector3f p3) {
        detail::quad(m, {p0, n, {0, 0}, {1, 1, 1, 1}}, {p1, n, {1, 0}, {1, 1, 1, 1}}, {p2, n, {1, 1}, {1, 1, 1, 1}}, {p3, n, {0, 1}, {1, 1, 1, 1}});
    };
    face({0, 0, 1}, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z});
    face({0, 0, -1}, {x, -y, -z}, {-x, -y, -z}, {-x, y, -z}, {x, y, -z});
    face({1, 0, 0}, {x, -y, z}, {x, -y, -z}, {x, y, -z}, {x, y, z});
    face({-1, 0, 0}, {-x, -y, -z}, {-x, -y, z}, {-x, y, z}, {-x, y, -z});
    face({0, 1, 0}, {-x, y, z}, {x, y, z}, {x, y, -z}, {-x, y, -z});
    face({0, -1, 0}, {-x, -y, -z}, {x, -y, -z}, {x, -y, z}, {-x, -y, z});
    return m;
}

/// Flat grid in the XZ plane facing +Y.
[[nodiscard]] inline Mesh plane(float width = 1, float depth = 1, u32 sx = 1, u32 sz = 1) {
    if (sx == 0 || sz == 0) throw GraphicsException("plane needs at least one subdivision");
    Mesh m;
    for (u32 j = 0; j <= sz; ++j)
        for (u32 i = 0; i <= sx; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(sx), v = static_cast<float>(j) / static_cast<float>(sz);
            m.vertices.push_back({{(u - 0.5f) * width, 0, (v - 0.5f) * depth}, {0, 1, 0}, {u, v}, {1, 1, 1, 1}});
        }
    for (u32 j = 0; j < sz; ++j)
        for (u32 i = 0; i < sx; ++i) {
            const u32 a = j * (sx + 1) + i, b = a + 1, c = a + sx + 1, d = c + 1;
            for (u32 k : {a, c, b, b, c, d}) m.indices.push_back(k);
        }
    return m;
}

[[nodiscard]] inline Mesh sphere(float radius = 0.5f, u32 segments = 32, u32 rings = 16) {
    if (segments < 3 || rings < 2) throw GraphicsException("sphere needs >= 3 segments and >= 2 rings");
    Mesh m;
    for (u32 r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings), phi = v * kPi;
        for (u32 s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments), th = u * 2 * kPi;
            const Vector3f n{std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th)};
            m.vertices.push_back({n * radius, n, {u, v}, {1, 1, 1, 1}});
        }
    }
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + 1, c = a + segments + 1, d = c + 1;
            if (r != 0) for (u32 k : {a, b, c}) m.indices.push_back(k);
            if (r != rings - 1) for (u32 k : {b, d, c}) m.indices.push_back(k);
        }
    return m;
}

/// Cylinder along Y (centred), optionally with caps.
[[nodiscard]] inline Mesh cylinder(float radius = 0.5f, float height = 1, u32 segments = 32, bool caps = true) {
    if (segments < 3) throw GraphicsException("cylinder needs >= 3 segments");
    Mesh m;
    const float h = height / 2;
    for (u32 s = 0; s <= segments; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(segments), th = u * 2 * kPi;
        const Vector3f n{std::cos(th), 0, std::sin(th)};
        m.vertices.push_back({{n.x * radius, -h, n.z * radius}, n, {u, 1}, {1, 1, 1, 1}});
        m.vertices.push_back({{n.x * radius, h, n.z * radius}, n, {u, 0}, {1, 1, 1, 1}});
    }
    for (u32 s = 0; s < segments; ++s) {
        const u32 a = s * 2, b = a + 1, c = a + 2, d = a + 3;
        for (u32 k : {a, c, b, b, c, d}) m.indices.push_back(k);
    }
    if (caps) {
        for (int top = 0; top < 2; ++top) {
            const float y = top ? h : -h;
            const Vector3f n{0, top ? 1.0f : -1.0f, 0};
            const u32 center = static_cast<u32>(m.vertices.size());
            m.vertices.push_back({{0, y, 0}, n, {0.5f, 0.5f}, {1, 1, 1, 1}});
            for (u32 s = 0; s <= segments; ++s) {
                const float th = static_cast<float>(s) / static_cast<float>(segments) * 2 * kPi;
                m.vertices.push_back({{std::cos(th) * radius, y, std::sin(th) * radius}, n, {0.5f + 0.5f * std::cos(th), 0.5f + 0.5f * std::sin(th)}, {1, 1, 1, 1}});
            }
            for (u32 s = 0; s < segments; ++s) {
                if (top) for (u32 k : {center, center + s + 2, center + s + 1}) m.indices.push_back(k);
                else for (u32 k : {center, center + s + 1, center + s + 2}) m.indices.push_back(k);
            }
        }
    }
    return m;
}

[[nodiscard]] inline Mesh cone(float radius = 0.5f, float height = 1, u32 segments = 32, bool cap = true) {
    if (segments < 3) throw GraphicsException("cone needs >= 3 segments");
    Mesh m;
    const float h = height / 2, slope = radius / height;
    for (u32 s = 0; s <= segments; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(segments), th = u * 2 * kPi;
        Vector3f n{std::cos(th), slope, std::sin(th)};
        n = n * (1.0f / math::length(n));
        m.vertices.push_back({{std::cos(th) * radius, -h, std::sin(th) * radius}, n, {u, 1}, {1, 1, 1, 1}});
        m.vertices.push_back({{0, h, 0}, n, {u, 0}, {1, 1, 1, 1}});
    }
    for (u32 s = 0; s < segments; ++s)
        for (u32 k : {s * 2, s * 2 + 2, s * 2 + 1}) m.indices.push_back(k);
    if (cap) {
        const u32 center = static_cast<u32>(m.vertices.size());
        m.vertices.push_back({{0, -h, 0}, {0, -1, 0}, {0.5f, 0.5f}, {1, 1, 1, 1}});
        for (u32 s = 0; s <= segments; ++s) {
            const float th = static_cast<float>(s) / static_cast<float>(segments) * 2 * kPi;
            m.vertices.push_back({{std::cos(th) * radius, -h, std::sin(th) * radius}, {0, -1, 0}, {0.5f + 0.5f * std::cos(th), 0.5f + 0.5f * std::sin(th)}, {1, 1, 1, 1}});
        }
        for (u32 s = 0; s < segments; ++s) for (u32 k : {center, center + s + 1, center + s + 2}) m.indices.push_back(k);
    }
    return m;
}

/// Torus around Y.
[[nodiscard]] inline Mesh torus(float major = 0.5f, float minor = 0.2f, u32 segments = 32, u32 tube_segments = 16) {
    if (segments < 3 || tube_segments < 3) throw GraphicsException("torus needs >= 3 segments");
    Mesh m;
    for (u32 i = 0; i <= segments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(segments), th = u * 2 * kPi;
        for (u32 j = 0; j <= tube_segments; ++j) {
            const float v = static_cast<float>(j) / static_cast<float>(tube_segments), ph = v * 2 * kPi;
            const Vector3f n{std::cos(th) * std::cos(ph), std::sin(ph), std::sin(th) * std::cos(ph)};
            const Vector3f p{std::cos(th) * major, 0, std::sin(th) * major};
            m.vertices.push_back({p + n * minor, n, {u, v}, {1, 1, 1, 1}});
        }
    }
    for (u32 i = 0; i < segments; ++i)
        for (u32 j = 0; j < tube_segments; ++j) {
            const u32 a = i * (tube_segments + 1) + j, b = a + 1, c = a + tube_segments + 1, d = c + 1;
            for (u32 k : {a, b, c, b, d, c}) m.indices.push_back(k);
        }
    return m;
}

}  // namespace primitives

// ---------------------------------------------------------------------------------------------------- OBJ / MTL
namespace obj {

struct Model {
    Mesh mesh;
    std::vector<Material> materials;
    std::vector<std::string> material_libraries;  ///< names from `mtllib` lines (load them with parse_mtl)
};

namespace detail {
inline std::vector<std::string_view> split(std::string_view l) {
    std::vector<std::string_view> t;
    usize i = 0;
    while (i < l.size()) {
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t' || l[i] == '\r')) ++i;
        const usize s = i;
        while (i < l.size() && l[i] != ' ' && l[i] != '\t' && l[i] != '\r') ++i;
        if (i > s) t.push_back(l.substr(s, i - s));
    }
    return t;
}
inline float num(std::string_view s, usize line) {
    float v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) throw DataException("obj: bad number '" + std::string(s) + "' on line " + std::to_string(line));
    return v;
}
inline long idx(std::string_view s, usize line) {
    long v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || v == 0) throw DataException("obj: bad index '" + std::string(s) + "' on line " + std::to_string(line));
    return v;
}
}  // namespace detail

/// Parse OBJ text. Faces with more than three vertices are triangulated as fans; missing normals are generated.
[[nodiscard]] inline Model parse(std::string_view text) {
    using namespace detail;
    Model model;
    std::vector<Vector3f> pos, nrm;
    std::vector<Vector2f> uvs;
    std::vector<Vector4f> cols;
    std::map<std::tuple<long, long, long>, u32> dedupe;
    bool any_normals = false;
    std::string cur_group = "default", cur_mat;
    auto close_group = [&]() {
        if (!model.mesh.groups.empty()) model.mesh.groups.back().index_count = model.mesh.indices.size() - model.mesh.groups.back().first_index;
    };
    auto open_group = [&]() {
        close_group();
        model.mesh.groups.push_back({cur_group, cur_mat, model.mesh.indices.size(), 0});
    };
    open_group();
    usize line_no = 0, p = 0;
    while (p <= text.size()) {
        usize e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(p, e - p);
        p = e + 1;
        ++line_no;
        const usize hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        const auto tk = split(line);
        if (tk.empty()) continue;
        const std::string_view c = tk[0];
        if (c == "v") {
            if (tk.size() < 4) throw DataException("obj: vertex needs 3 coordinates (line " + std::to_string(line_no) + ")");
            pos.push_back({num(tk[1], line_no), num(tk[2], line_no), num(tk[3], line_no)});
            cols.push_back(tk.size() >= 7 ? Vector4f{num(tk[4], line_no), num(tk[5], line_no), num(tk[6], line_no), 1} : Vector4f{1, 1, 1, 1});
        } else if (c == "vt") {
            if (tk.size() < 3) throw DataException("obj: texture coordinate needs 2 values (line " + std::to_string(line_no) + ")");
            uvs.push_back({num(tk[1], line_no), num(tk[2], line_no)});
        } else if (c == "vn") {
            if (tk.size() < 4) throw DataException("obj: normal needs 3 values (line " + std::to_string(line_no) + ")");
            nrm.push_back({num(tk[1], line_no), num(tk[2], line_no), num(tk[3], line_no)});
        } else if (c == "f") {
            if (tk.size() < 4) throw DataException("obj: face needs at least 3 vertices (line " + std::to_string(line_no) + ")");
            std::vector<u32> face;
            for (usize k = 1; k < tk.size(); ++k) {
                long ids[3] = {0, 0, 0};
                std::string_view s = tk[k];
                for (int part = 0; part < 3; ++part) {
                    const usize slash = s.find('/');
                    const std::string_view f = s.substr(0, slash);
                    if (!f.empty()) ids[part] = idx(f, line_no);
                    if (slash == std::string_view::npos) break;
                    s = s.substr(slash + 1);
                }
                auto resolve = [&](long v, usize n, const char* what) -> long {
                    if (v == 0) return -1;
                    const long r = v > 0 ? v - 1 : static_cast<long>(n) + v;
                    if (r < 0 || r >= static_cast<long>(n)) throw DataException(std::string("obj: ") + what + " index out of range (line " + std::to_string(line_no) + ")");
                    return r;
                };
                const long vi = resolve(ids[0], pos.size(), "vertex");
                if (vi < 0) throw DataException("obj: face vertex without position (line " + std::to_string(line_no) + ")");
                const long ti = resolve(ids[1], uvs.size(), "texcoord");
                const long ni = resolve(ids[2], nrm.size(), "normal");
                const auto key = std::make_tuple(vi, ti, ni);
                auto it = dedupe.find(key);
                if (it == dedupe.end()) {
                    Vertex v;
                    v.position = pos[static_cast<usize>(vi)];
                    v.color = cols[static_cast<usize>(vi)];
                    if (ti >= 0) v.uv = uvs[static_cast<usize>(ti)];
                    if (ni >= 0) { v.normal = nrm[static_cast<usize>(ni)]; any_normals = true; }
                    it = dedupe.emplace(key, static_cast<u32>(model.mesh.vertices.size())).first;
                    model.mesh.vertices.push_back(v);
                }
                face.push_back(it->second);
            }
            for (usize k = 1; k + 1 < face.size(); ++k) {
                model.mesh.indices.push_back(face[0]);
                model.mesh.indices.push_back(face[k]);
                model.mesh.indices.push_back(face[k + 1]);
            }
        } else if (c == "g" || c == "o") {
            cur_group = tk.size() > 1 ? std::string(tk[1]) : "default";
            if (model.mesh.indices.size() != model.mesh.groups.back().first_index) open_group();
            else model.mesh.groups.back().name = cur_group;
        } else if (c == "usemtl") {
            cur_mat = tk.size() > 1 ? std::string(tk[1]) : "";
            if (model.mesh.indices.size() != model.mesh.groups.back().first_index) open_group();
            else model.mesh.groups.back().material = cur_mat;
        } else if (c == "mtllib") {
            for (usize k = 1; k < tk.size(); ++k) model.material_libraries.emplace_back(tk[k]);
        }
    }
    close_group();
    model.mesh.groups.erase(std::remove_if(model.mesh.groups.begin(), model.mesh.groups.end(), [](const MeshGroup& g) { return g.index_count == 0; }), model.mesh.groups.end());
    if (model.mesh.indices.empty()) throw DataException("obj: no faces found");
    if (!any_normals) model.mesh.compute_normals();
    else {
        for (Vertex& v : model.mesh.vertices) {  // vertices without a vn keep the default; renormalise the rest
            const float l = math::length(v.normal);
            if (l > 1e-20f) v.normal = v.normal * (1.0f / l);
        }
    }
    return model;
}

/// Parse MTL text.
[[nodiscard]] inline std::vector<Material> parse_mtl(std::string_view text) {
    using namespace detail;
    std::vector<Material> out;
    usize p = 0, line_no = 0;
    while (p <= text.size()) {
        usize e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(p, e - p);
        p = e + 1;
        ++line_no;
        const usize hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        const auto tk = split(line);
        if (tk.empty()) continue;
        if (tk[0] == "newmtl") { out.emplace_back(); out.back().name = tk.size() > 1 ? std::string(tk[1]) : ""; continue; }
        if (out.empty()) continue;
        Material& m = out.back();
        auto vec3 = [&]() { if (tk.size() < 4) throw DataException("mtl: colour needs 3 values (line " + std::to_string(line_no) + ")"); return Vector3f{num(tk[1], line_no), num(tk[2], line_no), num(tk[3], line_no)}; };
        if (tk[0] == "Ka") m.ambient = vec3();
        else if (tk[0] == "Kd") m.diffuse = vec3();
        else if (tk[0] == "Ks") m.specular = vec3();
        else if (tk[0] == "Ke") m.emission = vec3();
        else if (tk[0] == "Ns" && tk.size() > 1) m.shininess = num(tk[1], line_no);
        else if (tk[0] == "d" && tk.size() > 1) m.opacity = num(tk[1], line_no);
        else if (tk[0] == "Tr" && tk.size() > 1) m.opacity = 1 - num(tk[1], line_no);
        else if (tk[0] == "map_Kd" && tk.size() > 1) m.diffuse_map = std::string(tk.back());
        else if ((tk[0] == "map_Bump" || tk[0] == "bump" || tk[0] == "map_bump") && tk.size() > 1) m.normal_map = std::string(tk.back());
    }
    return out;
}

[[nodiscard]] inline Model load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw DataException("obj: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

}  // namespace obj
}  // namespace flyg::graphics
