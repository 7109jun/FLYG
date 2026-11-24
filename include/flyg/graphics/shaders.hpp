// flyG - graphics module: camera and ready-made shaders for the software 3D renderer (unlit, Blinn-Phong, normals)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <vector>

#include <flyg/graphics/mesh.hpp>
#include <flyg/graphics/renderer.hpp>
#include <flyg/graphics/texture.hpp>

namespace flyg::graphics {

struct Camera {
    Vector3f eye{0, 0, 3}, target{0, 0, 0}, up{0, 1, 0};
    float fov_y = 0.9f;  ///< radians
    float z_near = 0.1f, z_far = 100.0f;
    bool orthographic = false;
    float ortho_height = 4.0f;  ///< visible world height for orthographic projection

    [[nodiscard]] Matrix4f view() const { return math::look_at(eye, target, up); }
    [[nodiscard]] Matrix4f projection(float aspect) const {
        if (orthographic) {
            const float h = ortho_height / 2, w = h * aspect;
            return math::orthographic(-w, w, -h, h, z_near, z_far);
        }
        return math::perspective(fov_y, aspect, z_near, z_far);
    }
    [[nodiscard]] Matrix4f view_projection(float aspect) const { return projection(aspect) * view(); }
};

struct Light {
    enum class Type { directional, point } type = Type::directional;
    Vector3f direction{-0.4f, -1.0f, -0.6f};  ///< directional: direction the light travels
    Vector3f position{0, 5, 0};               ///< point light position
    Vector3f color{1, 1, 1};
    float intensity = 1.0f;
    float attenuation = 0.0f;                 ///< point light: 1 / (1 + k * d^2)
};

/// Interpolated attributes shared by the built-in shaders.
struct StandardVarying {
    Vector3f world_pos{0, 0, 0}, normal{0, 0, 0};
    Vector2f uv{0, 0};
    Vector4f color{0, 0, 0, 0};
    friend StandardVarying operator+(const StandardVarying& a, const StandardVarying& b) { return {a.world_pos + b.world_pos, a.normal + b.normal, a.uv + b.uv, a.color + b.color}; }
    friend StandardVarying operator*(const StandardVarying& a, float s) { return {a.world_pos * s, a.normal * s, a.uv * s, a.color * s}; }
};

namespace detail_shader {
inline StandardVarying make_varying(const Vertex& v, const Matrix4f& model, const Matrix4f& normal_matrix) {
    StandardVarying o;
    o.world_pos = math::transform_point(model, v.position);
    o.normal = math::transform_direction(normal_matrix, v.normal);
    o.uv = v.uv;
    o.color = v.color;
    return o;
}
}  // namespace detail_shader

/// Flat colour * optional texture * optional vertex colour.
class UnlitShader {
public:
    using Varying = StandardVarying;
    static constexpr bool needs_derivatives = true;

    Matrix4f model = Matrix4f::identity();
    Matrix4f view_projection = Matrix4f::identity();
    ColorF color{1, 1, 1, 1};            ///< linear light
    const Texture* texture = nullptr;
    bool use_vertex_color = false;
    float alpha_cutoff = 0.0f;           ///< fragments with alpha below this are discarded

    Vector4f vertex(const Vertex& v, Varying& out) const {
        out = detail_shader::make_varying(v, model, Matrix4f::identity());
        return view_projection * Vector4f(out.world_pos, 1.0f);
    }
    std::optional<ColorF> fragment(const Varying& in, const Varying& ddx, const Varying& ddy) const {
        ColorF c = color;
        if (texture) c = c * texture->sample(in.uv, ddx.uv, ddy.uv);
        if (use_vertex_color) c = c * ColorF{in.color.x, in.color.y, in.color.z, in.color.w};
        if (c.a < alpha_cutoff) return std::nullopt;
        return c;
    }
};

/// Blinn-Phong lighting with ambient, directional/point lights, diffuse texture, specular highlights and emission.
class BlinnPhongShader {
public:
    using Varying = StandardVarying;
    static constexpr bool needs_derivatives = true;

    Matrix4f model = Matrix4f::identity();
    Matrix4f view_projection = Matrix4f::identity();
    Vector3f eye{0, 0, 0};
    std::vector<Light> lights;
    Vector3f ambient_light{0.1f, 0.1f, 0.1f};
    Material material;                   ///< colours are interpreted in linear light
    const Texture* diffuse_texture = nullptr;
    bool use_vertex_color = false;
    bool two_sided = false;              ///< flip the normal for back faces (use with CullMode::none)
    float alpha_cutoff = 0.0f;

    /// Set model matrix and refresh the normal matrix (inverse transpose).
    void set_model(const Matrix4f& m) { model = m; normal_matrix_ = math::transpose(math::inverse(m)); }
    void set_camera(const Camera& cam, float aspect) { view_projection = cam.view_projection(aspect); eye = cam.eye; }

    Vector4f vertex(const Vertex& v, Varying& out) const {
        out = detail_shader::make_varying(v, model, normal_matrix_);
        return view_projection * Vector4f(out.world_pos, 1.0f);
    }
    std::optional<ColorF> fragment(const Varying& in, const Varying& ddx, const Varying& ddy) const {
        ColorF base{material.diffuse.x, material.diffuse.y, material.diffuse.z, material.opacity};
        if (diffuse_texture) base = base * diffuse_texture->sample(in.uv, ddx.uv, ddy.uv);
        if (use_vertex_color) base = base * ColorF{in.color.x, in.color.y, in.color.z, in.color.w};
        if (base.a < alpha_cutoff) return std::nullopt;
        const float nl = math::length(in.normal);
        Vector3f n = nl > 1e-12f ? in.normal * (1.0f / nl) : Vector3f{0, 1, 0};
        const Vector3f to_eye_raw = eye - in.world_pos;
        const float el = math::length(to_eye_raw);
        const Vector3f V = el > 1e-12f ? to_eye_raw * (1.0f / el) : Vector3f{0, 0, 1};
        if (two_sided && math::dot(n, V) < 0) n = n * -1.0f;
        Vector3f diffuse_sum = ambient_light;  // scaled by the base colour below
        Vector3f spec_sum{0, 0, 0};
        for (const Light& l : lights) {
            Vector3f L;
            float att = 1.0f;
            if (l.type == Light::Type::directional) {
                const float dl = math::length(l.direction);
                L = dl > 1e-12f ? l.direction * (-1.0f / dl) : Vector3f{0, 1, 0};
            } else {
                const Vector3f d = l.position - in.world_pos;
                const float dist2 = math::dot(d, d), dist = std::sqrt(dist2);
                L = dist > 1e-12f ? d * (1.0f / dist) : Vector3f{0, 1, 0};
                att = 1.0f / (1.0f + l.attenuation * dist2);
            }
            const float ndl = std::max(0.0f, math::dot(n, L));
            if (ndl <= 0) continue;
            const Vector3f lc = l.color * (l.intensity * att);
            diffuse_sum += lc * ndl;
            const Vector3f H = L + V;
            const float hl = math::length(H);
            if (hl > 1e-12f && material.shininess > 0) {
                const float ndh = std::max(0.0f, math::dot(n, H * (1.0f / hl)));
                spec_sum += lc * std::pow(ndh, material.shininess);
            }
        }
        const Vector3f spec = {spec_sum.x * material.specular.x, spec_sum.y * material.specular.y, spec_sum.z * material.specular.z};
        return ColorF{base.r * diffuse_sum.x + spec.x + material.emission.x, base.g * diffuse_sum.y + spec.y + material.emission.y,
                      base.b * diffuse_sum.z + spec.z + material.emission.z, base.a};
    }

private:
    Matrix4f normal_matrix_ = Matrix4f::identity();
};

/// Debug shader: world-space normal mapped to colour.
class NormalShader {
public:
    using Varying = StandardVarying;
    Matrix4f model = Matrix4f::identity();
    Matrix4f view_projection = Matrix4f::identity();
    void set_model(const Matrix4f& m) { model = m; normal_matrix_ = math::transpose(math::inverse(m)); }
    Vector4f vertex(const Vertex& v, Varying& out) const {
        out = detail_shader::make_varying(v, model, normal_matrix_);
        return view_projection * Vector4f(out.world_pos, 1.0f);
    }
    std::optional<ColorF> fragment(const Varying& in) const {
        const float l = math::length(in.normal);
        const Vector3f n = l > 1e-12f ? in.normal * (1.0f / l) : Vector3f{0, 0, 0};
        return ColorF{n.x * 0.5f + 0.5f, n.y * 0.5f + 0.5f, n.z * 0.5f + 0.5f, 1.0f};
    }
private:
    Matrix4f normal_matrix_ = Matrix4f::identity();
};

}  // namespace flyg::graphics
