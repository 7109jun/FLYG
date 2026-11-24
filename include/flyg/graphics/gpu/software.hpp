// flyG - graphics module: software GPU backend (executes CommandLists on the CPU with the Renderer)
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <map>

#include <flyg/graphics/gpu/gpu.hpp>

namespace flyg::graphics::gpu {

namespace sw_detail {

class SwBuffer final : public Buffer {
public:
    SwBuffer(usize size, BufferUsage u, std::span<const std::byte> init) : Buffer(size, u), data_(size) {
        if (init.size() > size) throw GraphicsException("initial data larger than the buffer");
        if (!init.empty()) std::memcpy(data_.data(), init.data(), init.size());
    }
    void write(usize offset, std::span<const std::byte> d) override {
        check(offset, d.size());
        if (!d.empty()) std::memcpy(data_.data() + offset, d.data(), d.size());
    }
    [[nodiscard]] std::vector<std::byte> read(usize offset, usize n) const override {
        check(offset, n);
        return {data_.begin() + static_cast<std::ptrdiff_t>(offset), data_.begin() + static_cast<std::ptrdiff_t>(offset + n)};
    }
    [[nodiscard]] const std::byte* data() const noexcept { return data_.data(); }
private:
    std::vector<std::byte> data_;
};

class SwTexture final : public GpuTexture {
public:
    explicit SwTexture(const TextureDesc& d, std::span<const std::byte> init) : GpuTexture(d), px_(d.width * d.height * bytes_per_pixel(d.format)) {
        if (d.width == 0 || d.height == 0 || d.width > 16384 || d.height > 16384) throw GraphicsException("invalid texture size");
        if (!init.empty()) write(init);
    }
    void write(std::span<const std::byte> pixels) override {
        if (pixels.size() != px_.size()) throw GraphicsException("texture upload: wrong data size");
        std::memcpy(px_.data(), pixels.data(), pixels.size());
        ++version_;
    }
    using GpuTexture::write;
    [[nodiscard]] Image read() const override {
        Image img(w_, h_);
        for (usize i = 0; i < img.pixel_count(); ++i) {
            if (fmt_ == PixelFormat::r8_unorm) img.data()[i] = Color{static_cast<u8>(px_[i]), 0, 0, 255};
            else img.data()[i] = Color{static_cast<u8>(px_[4 * i]), static_cast<u8>(px_[4 * i + 1]), static_cast<u8>(px_[4 * i + 2]), static_cast<u8>(px_[4 * i + 3])};
        }
        return img;
    }
    void store(const Image& img) {  // render target write-back
        for (usize i = 0; i < img.pixel_count(); ++i) {
            const Color c = img.data()[i];
            if (fmt_ == PixelFormat::r8_unorm) px_[i] = static_cast<std::byte>(c.r);
            else { px_[4 * i] = static_cast<std::byte>(c.r), px_[4 * i + 1] = static_cast<std::byte>(c.g), px_[4 * i + 2] = static_cast<std::byte>(c.b), px_[4 * i + 3] = static_cast<std::byte>(c.a); }
        }
        ++version_;
    }
    [[nodiscard]] u64 version() const noexcept { return version_; }
private:
    std::vector<std::byte> px_;
    u64 version_ = 1;
};

class SwPipeline final : public Pipeline {
public:
    explicit SwPipeline(PipelineDesc d) : Pipeline(std::move(d)) {}
};

class SwFramebuffer final : public Framebuffer {
public:
    SwFramebuffer(std::shared_ptr<GpuTexture> c, bool depth) : Framebuffer(std::move(c), depth), renderer(color_->width(), color_->height(), 1) {}
    Renderer renderer;
};

struct RawVertex {
    const std::byte* data;
    u32 id;
};

/// Adapts a SoftwareShader to the Renderer's shader interface (including the viewport transform).
struct Adapter {
    using Varying = VaryingBlock;
    static constexpr bool needs_derivatives = true;
    const PipelineDesc* desc;
    const Bindings* bindings;
    const VertexLayout* layout;
    // viewport transform: ndc' = ndc * scale + bias
    float sx = 1, sy = 1, bx = 0, by = 0;

    Vector4f vertex(const RawVertex& v, Varying& out) const {
        Vector4f clip = desc->shader.software.vertex(VertexInput(v.data, layout, v.id), *bindings, out);
        const float ndc_x = clip.x, ndc_y = clip.y;  // clip-space values; the viewport remap is linear in (x/w, y/w)
        clip.x = ndc_x * sx + clip.w * bx;
        clip.y = ndc_y * sy + clip.w * by;
        return clip;
    }
    std::optional<ColorF> fragment(const Varying& v, const Varying& dx, const Varying& dy) const {
        return desc->shader.software.fragment(FragmentInput{v, dx, dy, *bindings});
    }
};

}  // namespace sw_detail

/// CPU implementation of the GPU abstraction.
class SoftwareDevice final : public Device {
public:
    [[nodiscard]] DeviceInfo info() const override { return {Backend::software, "flyG software rasteriser", "flyG", "1.0"}; }

    std::shared_ptr<Buffer> create_buffer(const BufferDesc& d, std::span<const std::byte> initial = {}) override {
        if (d.size == 0) throw GraphicsException("buffer size must be non-zero");
        return std::make_shared<sw_detail::SwBuffer>(d.size, d.usage, initial);
    }
    std::shared_ptr<GpuTexture> create_texture(const TextureDesc& d, std::span<const std::byte> initial = {}) override {
        return std::make_shared<sw_detail::SwTexture>(d, initial);
    }
    std::shared_ptr<Pipeline> create_pipeline(const PipelineDesc& d) override {
        if (!d.shader.software.vertex || !d.shader.software.fragment) throw GraphicsException("software backend needs SoftwareShader vertex and fragment functions");
        if (d.layout.stride == 0) throw GraphicsException("pipeline vertex layout has a zero stride");
        for (const auto& a : d.layout.attributes)
            if (a.offset + vertex_format_size(a.format) > d.layout.stride) throw GraphicsException("vertex attribute exceeds the vertex stride");
        return std::make_shared<sw_detail::SwPipeline>(d);
    }
    std::shared_ptr<Framebuffer> create_framebuffer(std::shared_ptr<GpuTexture> color, bool with_depth) override {
        if (!color || !color->is_render_target()) throw GraphicsException("framebuffer colour attachment must be a render-target texture");
        if (color->format() == PixelFormat::r8_unorm) throw GraphicsException("r8 textures cannot be rendered to");
        return std::make_shared<sw_detail::SwFramebuffer>(std::move(color), with_depth);
    }
    void finish() override {}

    void submit(const CommandList& list) override {
        if (!list.complete()) throw GraphicsException("command list has an unfinished render pass");
        using namespace sw_detail;
        SwFramebuffer* fb = nullptr;
        std::shared_ptr<Framebuffer> fb_keep;
        const SwPipeline* pipe = nullptr;
        std::shared_ptr<Pipeline> pipe_keep;
        std::shared_ptr<Buffer> vb, ib;
        usize vb_off = 0, ib_off = 0;
        IndexType itype = IndexType::u16;
        Bindings bindings;
        std::array<std::shared_ptr<Buffer>, kMaxBindSlots> ub_keep;
        std::vector<std::unique_ptr<Texture>> tex_keep;
        std::array<float, 4> vp{0, 0, 0, 0};
        bool vp_set = false;
        for (const Command& c : list.commands()) {
            if (const auto* b = std::get_if<cmd::BeginPass>(&c)) {
                fb_keep = b->pass.target;
                fb = dynamic_cast<SwFramebuffer*>(fb_keep.get());
                if (!fb) throw GraphicsException("framebuffer belongs to a different backend");
                auto* tex = static_cast<SwTexture*>(fb->color().get());
                if (b->pass.clear_color) fb->renderer.clear_color(*b->pass.clear_color);
                else fb->renderer.load_color(tex->read());
                if (b->pass.clear_depth && fb->has_depth()) fb->renderer.clear_depth(*b->pass.clear_depth);
                vp_set = false;
                pipe = nullptr;
                bindings = Bindings{};
                tex_keep.clear();
            } else if (std::holds_alternative<cmd::EndPass>(c)) {
                static_cast<SwTexture*>(fb->color().get())->store(fb->renderer.resolve());
                fb = nullptr;
            } else if (const auto* p = std::get_if<cmd::SetPipeline>(&c)) {
                pipe_keep = p->pipeline;
                pipe = dynamic_cast<const SwPipeline*>(pipe_keep.get());
                if (!pipe) throw GraphicsException("pipeline belongs to a different backend");
            } else if (const auto* sv = std::get_if<cmd::SetVertexBuffer>(&c)) {
                vb = sv->buffer, vb_off = sv->offset;
            } else if (const auto* i = std::get_if<cmd::SetIndexBuffer>(&c)) {
                ib = i->buffer, itype = i->type, ib_off = i->offset;
            } else if (const auto* u = std::get_if<cmd::SetUniform>(&c)) {
                auto* sb = dynamic_cast<SwBuffer*>(u->buffer.get());
                if (!sb) throw GraphicsException("uniform buffer belongs to a different backend");
                const usize n = u->size ? u->size : sb->size() - std::min(u->offset, sb->size());
                if (u->offset + n > sb->size()) throw GraphicsException("uniform range outside the buffer");
                ub_keep[u->slot] = u->buffer;
                bindings.uniforms_[u->slot] = std::span<const std::byte>(sb->data() + u->offset, n);
            } else if (const auto* t = std::get_if<cmd::SetTexture>(&c)) {
                auto* st = dynamic_cast<SwTexture*>(t->texture.get());
                if (!st) throw GraphicsException("texture belongs to a different backend");
                auto tex = std::make_unique<Texture>(st->read(), t->sampler.mipmaps, st->format() == PixelFormat::rgba8_srgb);
                tex->wrap_s = t->sampler.wrap_s, tex->wrap_t = t->sampler.wrap_t;
                const bool nearest = t->sampler.min == FilterMode::nearest && t->sampler.mag == FilterMode::nearest;
                tex->filter = nearest ? TexFilter::nearest : (t->sampler.mipmaps && t->sampler.min == FilterMode::linear ? TexFilter::trilinear : TexFilter::bilinear);
                bindings.textures_[t->slot] = tex.get();
                tex_keep.push_back(std::move(tex));
            } else if (const auto* sp = std::get_if<cmd::SetViewport>(&c)) {
                vp = {sp->x, sp->y, sp->w, sp->h};
                vp_set = true;
            } else if (const auto* dr = std::get_if<cmd::Draw>(&c)) {
                draw(fb, pipe, vb.get(), vb_off, nullptr, itype, 0, bindings, vp, vp_set, dr->vertex_count, dr->first_vertex, 0);
            } else if (const auto* di = std::get_if<cmd::DrawIndexed>(&c)) {
                if (!ib) throw GraphicsException("draw_indexed without an index buffer");
                draw(fb, pipe, vb.get(), vb_off, ib.get(), itype, ib_off, bindings, vp, vp_set, di->index_count, di->first_index, di->base_vertex);
            }
        }
    }

private:
    static void draw(sw_detail::SwFramebuffer* fb, const sw_detail::SwPipeline* pipe, Buffer* vbuf, usize vb_off, Buffer* ibuf, IndexType itype, usize ib_off,
                     const Bindings& bindings, const std::array<float, 4>& vp, bool vp_set, u32 count, u32 first, i32 base_vertex) {
        using namespace sw_detail;
        if (!fb || !pipe) throw GraphicsException("draw without a render pass and pipeline");
        if (!vbuf) throw GraphicsException("draw without a vertex buffer");
        const PipelineDesc& d = pipe->desc();
        const auto* vb = static_cast<SwBuffer*>(vbuf);
        const u32 stride = d.layout.stride;
        // gather indices (as triangle list)
        std::vector<u32> raw;
        raw.reserve(count);
        if (ibuf) {
            const auto* ib = static_cast<SwBuffer*>(ibuf);
            const usize isz = itype == IndexType::u16 ? 2 : 4;
            if (ib_off + (usize(first) + count) * isz > ib->size()) throw GraphicsException("index range outside the index buffer");
            for (u32 i = 0; i < count; ++i) {
                const std::byte* p = ib->data() + ib_off + (usize(first) + i) * isz;
                u32 v;
                if (itype == IndexType::u16) { u16 s; std::memcpy(&s, p, 2); v = s; } else std::memcpy(&v, p, 4);
                const i64 id = static_cast<i64>(v) + base_vertex;
                if (id < 0) throw GraphicsException("negative vertex index");
                raw.push_back(static_cast<u32>(id));
            }
        } else {
            for (u32 i = 0; i < count; ++i) raw.push_back(first + i);
        }
        std::vector<u32> tris;
        switch (d.topology) {
            case Topology::triangles: tris = raw; tris.resize(tris.size() / 3 * 3); break;
            case Topology::triangle_strip:
                for (usize i = 0; i + 2 < raw.size(); ++i) {
                    if (i % 2 == 0) tris.insert(tris.end(), {raw[i], raw[i + 1], raw[i + 2]});
                    else tris.insert(tris.end(), {raw[i + 1], raw[i], raw[i + 2]});
                }
                break;
            case Topology::triangle_fan:
                for (usize i = 1; i + 1 < raw.size(); ++i) tris.insert(tris.end(), {raw[0], raw[i], raw[i + 1]});
                break;
        }
        if (tris.empty()) return;
        u32 max_index = 0;
        for (u32 i : tris) max_index = std::max(max_index, i);
        if (vb_off + (usize(max_index) + 1) * stride > vb->size()) throw GraphicsException("draw reads outside the vertex buffer");
        std::vector<RawVertex> verts(usize(max_index) + 1);
        for (u32 i = 0; i <= max_index; ++i) verts[i] = {vb->data() + vb_off + usize(i) * stride, i};

        DrawState st;
        st.cull = d.cull, st.front_ccw = d.front_ccw;
        st.depth_test = d.depth_test && fb->has_depth();
        st.depth_write = d.depth_write && fb->has_depth();
        st.depth_func = d.depth_func;
        st.blend = d.blend;
        st.polygon = d.polygon;
        st.linear_output = fb->color()->format() == PixelFormat::rgba8_srgb;
        Adapter ad{&d, &bindings, &d.layout};
        const float W = static_cast<float>(fb->width()), H = static_cast<float>(fb->height());
        if (vp_set) {
            ad.sx = vp[2] / W;
            ad.bx = (2 * vp[0] + vp[2]) / W - 1;
            ad.sy = vp[3] / H;
            ad.by = 1 - 2 * vp[1] / H - vp[3] / H;
            st.scissor = true;
            st.scissor_x = static_cast<long>(std::floor(vp[0])), st.scissor_y = static_cast<long>(std::floor(vp[1]));
            st.scissor_w = static_cast<long>(std::ceil(vp[2])), st.scissor_h = static_cast<long>(std::ceil(vp[3]));
        }
        fb->renderer.draw(std::span<const RawVertex>(verts), std::span<const u32>(tris), ad, st);
    }
};

[[nodiscard]] inline std::unique_ptr<Device> create_software_device() { return std::make_unique<SoftwareDevice>(); }

}  // namespace flyg::graphics::gpu
