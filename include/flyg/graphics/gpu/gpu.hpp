// flyG - graphics module: GPU abstraction (Device / Buffer / Texture / Pipeline / Framebuffer / CommandList).
// One command-recording API; backends: software (always available), OpenGL (optional, FLYG_GPU_OPENGL).
//
// Conventions (identical on every backend):
//   * clip space: right-handed, y up, z in [-1,1]; window origin is the top-left of the image, row 0 = top.
//   * counter-clockwise triangles are front facing (Pipeline::front_ccw flips this).
//   * fragment colours are written as-is to rgba8_unorm targets and sRGB-encoded for rgba8_srgb targets.
//   * textures: row 0 of the uploaded data is v = 0; rgba8_srgb textures are decoded to linear light when sampled.
//   * all device calls must be made from the thread that created the device.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <flyg/graphics/mesh.hpp>
#include <flyg/graphics/renderer.hpp>
#include <flyg/graphics/texture.hpp>

namespace flyg::graphics::gpu {

enum class Backend { software, opengl };
enum class BufferUsage : u8 { vertex, index, uniform };
enum class PixelFormat : u8 { rgba8_unorm, rgba8_srgb, r8_unorm };
enum class IndexType : u8 { u16, u32 };
enum class Topology : u8 { triangles, triangle_strip, triangle_fan };
enum class VertexFormat : u8 { float1, float2, float3, float4, unorm8x4 };
enum class FilterMode : u8 { nearest, linear };

[[nodiscard]] inline usize bytes_per_pixel(PixelFormat f) noexcept { return f == PixelFormat::r8_unorm ? 1 : 4; }
[[nodiscard]] inline usize vertex_format_size(VertexFormat f) noexcept {
    switch (f) {
        case VertexFormat::float1: return 4;
        case VertexFormat::float2: return 8;
        case VertexFormat::float3: return 12;
        case VertexFormat::float4: return 16;
        case VertexFormat::unorm8x4: return 4;
    }
    return 0;
}

struct BufferDesc {
    usize size = 0;
    BufferUsage usage = BufferUsage::vertex;
};

struct TextureDesc {
    usize width = 1, height = 1;
    PixelFormat format = PixelFormat::rgba8_unorm;
    bool render_target = false;
};

struct SamplerDesc {
    FilterMode min = FilterMode::linear, mag = FilterMode::linear;
    bool mipmaps = true;
    Wrap wrap_s = Wrap::repeat, wrap_t = Wrap::repeat;
};

struct VertexAttribute {
    u32 location = 0;
    VertexFormat format = VertexFormat::float3;
    u32 offset = 0;
};
struct VertexLayout {
    u32 stride = 0;
    std::vector<VertexAttribute> attributes;
};

/// Layout matching flyg::graphics::Vertex: position(0) normal(1) uv(2) color(3).
[[nodiscard]] inline VertexLayout standard_vertex_layout() {
    return {sizeof(Vertex), {{0, VertexFormat::float3, offsetof(Vertex, position)}, {1, VertexFormat::float3, offsetof(Vertex, normal)}, {2, VertexFormat::float2, offsetof(Vertex, uv)}, {3, VertexFormat::float4, offsetof(Vertex, color)}}};
}

// ------------------------------------------------------------------------------------------------ software shaders
inline constexpr usize kMaxVaryings = 16;

/// Floats passed from the vertex to the fragment stage (perspective-correct interpolated).
struct VaryingBlock {
    std::array<float, kMaxVaryings> f{};
    friend VaryingBlock operator+(const VaryingBlock& a, const VaryingBlock& b) { VaryingBlock r; for (usize i = 0; i < kMaxVaryings; ++i) r.f[i] = a.f[i] + b.f[i]; return r; }
    friend VaryingBlock operator*(const VaryingBlock& a, float s) { VaryingBlock r; for (usize i = 0; i < kMaxVaryings; ++i) r.f[i] = a.f[i] * s; return r; }
};

/// Read-only view of the attributes of one vertex.
class VertexInput {
public:
    VertexInput(const std::byte* data, const VertexLayout* layout, u32 id) : data_(data), layout_(layout), id_(id) {}
    [[nodiscard]] u32 vertex_id() const noexcept { return id_; }
    /// Attribute as vec4; missing components are 0 (w defaults to 1), missing attributes give (0,0,0,1).
    [[nodiscard]] Vector4f attribute(u32 location) const {
        for (const VertexAttribute& a : layout_->attributes) {
            if (a.location != location) continue;
            const std::byte* p = data_ + a.offset;
            float c[4] = {0, 0, 0, 1};
            if (a.format == VertexFormat::unorm8x4) {
                for (int i = 0; i < 4; ++i) c[i] = static_cast<float>(static_cast<u8>(p[i])) / 255.0f;
            } else {
                const int n = static_cast<int>(vertex_format_size(a.format) / 4);
                for (int i = 0; i < n; ++i) std::memcpy(&c[i], p + 4 * i, 4);
            }
            return {c[0], c[1], c[2], c[3]};
        }
        return {0, 0, 0, 1};
    }
private:
    const std::byte* data_;
    const VertexLayout* layout_;
    u32 id_;
};

inline constexpr usize kMaxBindSlots = 8;

/// Resources bound while drawing (what a GPU shader would see as uniform blocks and samplers).
class Bindings {
public:
    /// Read a trivially-copyable value from uniform slot `slot` at byte `offset`.
    template <class T>
    [[nodiscard]] T uniform(u32 slot, usize offset = 0) const {
        static_assert(std::is_trivially_copyable_v<T>);
        if (slot >= kMaxBindSlots || offset + sizeof(T) > uniforms_[slot].size()) throw GraphicsException("shader reads outside the bound uniform buffer");
        T v;
        std::memcpy(&v, uniforms_[slot].data() + offset, sizeof(T));
        return v;
    }
    /// Sample the texture bound to `slot` (linear light for sRGB textures). `lod` follows Texture::sample.
    [[nodiscard]] ColorF sample(u32 slot, Vector2f uv, float lod = 0) const { return tex(slot).sample(uv, lod); }
    [[nodiscard]] ColorF sample(u32 slot, Vector2f uv, Vector2f ddx, Vector2f ddy) const { return tex(slot).sample(uv, ddx, ddy); }

    std::array<std::span<const std::byte>, kMaxBindSlots> uniforms_{};
    std::array<const Texture*, kMaxBindSlots> textures_{};
private:
    [[nodiscard]] const Texture& tex(u32 slot) const {
        if (slot >= kMaxBindSlots || !textures_[slot]) throw GraphicsException("shader samples an unbound texture slot");
        return *textures_[slot];
    }
};

struct FragmentInput {
    const VaryingBlock& v;
    const VaryingBlock& ddx;
    const VaryingBlock& ddy;
    const Bindings& bindings;
};

/// Shader programs for the software backend: ordinary C++ callables.
struct SoftwareShader {
    std::function<Vector4f(const VertexInput&, const Bindings&, VaryingBlock&)> vertex;
    std::function<std::optional<ColorF>(const FragmentInput&)> fragment;
};

/// A pipeline needs the callable form for the software backend and/or GLSL source for the OpenGL backend.
struct ShaderSource {
    SoftwareShader software;
    std::string glsl_vertex, glsl_fragment;
    std::vector<std::string> uniform_blocks;  ///< OpenGL: name of the uniform block bound to slot i
    std::vector<std::string> samplers;        ///< OpenGL: name of the sampler2D uniform bound to texture slot i
};

struct PipelineDesc {
    ShaderSource shader;
    VertexLayout layout;
    Topology topology = Topology::triangles;
    CullMode cull = CullMode::back;
    bool front_ccw = true;
    bool depth_test = true, depth_write = true;
    DepthFunc depth_func = DepthFunc::less;
    bool blend = false;                       ///< source-over: (src_alpha, 1 - src_alpha)
    PolygonMode polygon = PolygonMode::fill;
};

// ------------------------------------------------------------------------------------------------ resources
class Buffer {
public:
    virtual ~Buffer() = default;
    [[nodiscard]] usize size() const noexcept { return size_; }
    [[nodiscard]] BufferUsage usage() const noexcept { return usage_; }
    virtual void write(usize offset, std::span<const std::byte> data) = 0;
    [[nodiscard]] virtual std::vector<std::byte> read(usize offset, usize size) const = 0;
protected:
    Buffer(usize size, BufferUsage u) : size_(size), usage_(u) {}
    void check(usize offset, usize n) const { if (offset > size_ || n > size_ - offset) throw GraphicsException("buffer access out of range"); }
    usize size_;
    BufferUsage usage_;
};

class GpuTexture {
public:
    virtual ~GpuTexture() = default;
    [[nodiscard]] usize width() const noexcept { return w_; }
    [[nodiscard]] usize height() const noexcept { return h_; }
    [[nodiscard]] PixelFormat format() const noexcept { return fmt_; }
    [[nodiscard]] bool is_render_target() const noexcept { return rt_; }
    /// Upload tightly packed pixels of the texture's format (row 0 first).
    virtual void write(std::span<const std::byte> pixels) = 0;
    /// Upload an RGBA image of the same size (converted for r8 textures by taking red).
    void write(const Image& img) {
        if (img.width() != w_ || img.height() != h_) throw GraphicsException("texture upload: size mismatch");
        std::vector<std::byte> raw(w_ * h_ * bytes_per_pixel(fmt_));
        for (usize i = 0; i < img.pixel_count(); ++i) {
            const Color c = img.data()[i];
            if (fmt_ == PixelFormat::r8_unorm) raw[i] = static_cast<std::byte>(c.r);
            else { raw[4 * i] = static_cast<std::byte>(c.r), raw[4 * i + 1] = static_cast<std::byte>(c.g), raw[4 * i + 2] = static_cast<std::byte>(c.b), raw[4 * i + 3] = static_cast<std::byte>(c.a); }
        }
        write(raw);
    }
    /// Read back as an RGBA8 image with the top row first (r8 textures give (r,0,0,255)).
    [[nodiscard]] virtual Image read() const = 0;
protected:
    GpuTexture(const TextureDesc& d) : w_(d.width), h_(d.height), fmt_(d.format), rt_(d.render_target) {}
    usize w_, h_;
    PixelFormat fmt_;
    bool rt_;
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    [[nodiscard]] const PipelineDesc& desc() const noexcept { return desc_; }
protected:
    explicit Pipeline(PipelineDesc d) : desc_(std::move(d)) {}
    PipelineDesc desc_;
};

class Framebuffer {
public:
    virtual ~Framebuffer() = default;
    [[nodiscard]] const std::shared_ptr<GpuTexture>& color() const noexcept { return color_; }
    [[nodiscard]] bool has_depth() const noexcept { return depth_; }
    [[nodiscard]] usize width() const noexcept { return color_->width(); }
    [[nodiscard]] usize height() const noexcept { return color_->height(); }
protected:
    Framebuffer(std::shared_ptr<GpuTexture> c, bool depth) : color_(std::move(c)), depth_(depth) {}
    std::shared_ptr<GpuTexture> color_;
    bool depth_;
};

// ------------------------------------------------------------------------------------------------ commands
struct PassDesc {
    std::shared_ptr<Framebuffer> target;
    std::optional<Color> clear_color = Color{0, 0, 0, 255};  ///< nullopt = keep existing contents
    std::optional<float> clear_depth = 1.0f;                 ///< nullopt = keep existing depth
};

namespace cmd {
struct BeginPass { PassDesc pass; };
struct EndPass {};
struct SetPipeline { std::shared_ptr<Pipeline> pipeline; };
struct SetVertexBuffer { std::shared_ptr<Buffer> buffer; usize offset; };
struct SetIndexBuffer { std::shared_ptr<Buffer> buffer; IndexType type; usize offset; };
struct SetUniform { u32 slot; std::shared_ptr<Buffer> buffer; usize offset, size; };
struct SetTexture { u32 slot; std::shared_ptr<GpuTexture> texture; SamplerDesc sampler; };
struct SetViewport { float x, y, w, h; };
struct Draw { u32 vertex_count, first_vertex; };
struct DrawIndexed { u32 index_count, first_index; i32 base_vertex; };
}  // namespace cmd

using Command = std::variant<cmd::BeginPass, cmd::EndPass, cmd::SetPipeline, cmd::SetVertexBuffer, cmd::SetIndexBuffer, cmd::SetUniform, cmd::SetTexture, cmd::SetViewport, cmd::Draw, cmd::DrawIndexed>;

/// Backend-independent recording of rendering commands; execute with Device::submit().
class CommandList {
public:
    CommandList& begin_pass(PassDesc p) {
        if (!p.target) throw GraphicsException("render pass without a target");
        if (in_pass_) throw GraphicsException("begin_pass inside a pass");
        in_pass_ = true;
        cmds_.push_back(cmd::BeginPass{std::move(p)});
        return *this;
    }
    CommandList& end_pass() {
        if (!in_pass_) throw GraphicsException("end_pass outside a pass");
        in_pass_ = false;
        cmds_.push_back(cmd::EndPass{});
        return *this;
    }
    CommandList& set_pipeline(std::shared_ptr<Pipeline> p) { need_pass(); cmds_.push_back(cmd::SetPipeline{std::move(p)}); return *this; }
    CommandList& set_vertex_buffer(std::shared_ptr<Buffer> b, usize offset = 0) { need_pass(); cmds_.push_back(cmd::SetVertexBuffer{std::move(b), offset}); return *this; }
    CommandList& set_index_buffer(std::shared_ptr<Buffer> b, IndexType t, usize offset = 0) { need_pass(); cmds_.push_back(cmd::SetIndexBuffer{std::move(b), t, offset}); return *this; }
    CommandList& set_uniform(u32 slot, std::shared_ptr<Buffer> b, usize offset = 0, usize size = 0) {
        need_pass();
        if (slot >= kMaxBindSlots) throw GraphicsException("uniform slot out of range");
        cmds_.push_back(cmd::SetUniform{slot, std::move(b), offset, size});
        return *this;
    }
    CommandList& set_texture(u32 slot, std::shared_ptr<GpuTexture> t, SamplerDesc s = {}) {
        need_pass();
        if (slot >= kMaxBindSlots) throw GraphicsException("texture slot out of range");
        cmds_.push_back(cmd::SetTexture{slot, std::move(t), s});
        return *this;
    }
    CommandList& set_viewport(float x, float y, float w, float h) { need_pass(); cmds_.push_back(cmd::SetViewport{x, y, w, h}); return *this; }
    CommandList& draw(u32 vertex_count, u32 first_vertex = 0) { need_pass(); cmds_.push_back(cmd::Draw{vertex_count, first_vertex}); return *this; }
    CommandList& draw_indexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) { need_pass(); cmds_.push_back(cmd::DrawIndexed{index_count, first_index, base_vertex}); return *this; }

    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return cmds_; }
    [[nodiscard]] bool complete() const noexcept { return !in_pass_; }
    void clear() { cmds_.clear(); in_pass_ = false; }

private:
    void need_pass() const { if (!in_pass_) throw GraphicsException("command recorded outside a render pass"); }
    std::vector<Command> cmds_;
    bool in_pass_ = false;
};

// ------------------------------------------------------------------------------------------------ device
struct DeviceInfo {
    Backend backend = Backend::software;
    std::string name, vendor, version;
};

class Device {
public:
    virtual ~Device() = default;
    [[nodiscard]] virtual DeviceInfo info() const = 0;
    [[nodiscard]] virtual std::shared_ptr<Buffer> create_buffer(const BufferDesc& d, std::span<const std::byte> initial = {}) = 0;
    [[nodiscard]] virtual std::shared_ptr<GpuTexture> create_texture(const TextureDesc& d, std::span<const std::byte> initial = {}) = 0;
    [[nodiscard]] virtual std::shared_ptr<Pipeline> create_pipeline(const PipelineDesc& d) = 0;
    [[nodiscard]] virtual std::shared_ptr<Framebuffer> create_framebuffer(std::shared_ptr<GpuTexture> color, bool with_depth) = 0;
    /// Execute a recorded command list (must be complete).
    virtual void submit(const CommandList& list) = 0;
    /// Wait until all submitted work has finished.
    virtual void finish() = 0;

    // conveniences
    template <class T>
    [[nodiscard]] std::shared_ptr<Buffer> create_buffer(BufferUsage u, std::span<const T> data) {
        static_assert(std::is_trivially_copyable_v<T>);
        return create_buffer(BufferDesc{data.size_bytes(), u}, std::as_bytes(data));
    }
    [[nodiscard]] std::shared_ptr<GpuTexture> create_texture(const Image& img, PixelFormat f = PixelFormat::rgba8_srgb) {
        auto t = create_texture(TextureDesc{img.width(), img.height(), f, false});
        t->write(img);
        return t;
    }
    [[nodiscard]] std::shared_ptr<Framebuffer> create_render_target(usize w, usize h, PixelFormat f = PixelFormat::rgba8_unorm, bool with_depth = true) {
        return create_framebuffer(create_texture(TextureDesc{w, h, f, true}), with_depth);
    }
};

}  // namespace flyg::graphics::gpu
