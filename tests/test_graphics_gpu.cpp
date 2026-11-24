// flyG - graphics module tests: GPU abstraction (software backend always, OpenGL backend when compiled in and a driver exists)
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/graphics/gpu/device.hpp>

#include <cstdio>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::graphics;
using namespace flyg::graphics::gpu;

namespace {

struct V {
    float x, y, z;
    float r, g, b, a;
};
static_assert(sizeof(V) == 28);

VertexLayout vlayout() {
    return {sizeof(V), {{0, VertexFormat::float3, 0}, {1, VertexFormat::float4, 12}}};
}

struct Tint { float r, g, b, a; };

std::string glsl_header(const Device& d) {
    return d.info().version.find("OpenGL ES") != std::string::npos ? "#version 300 es\nprecision highp float;\n" : "#version 330 core\n";
}

/// colour * optional tint uniform (slot 0, block "U")
PipelineDesc color_pipeline(const Device& d, bool use_tint) {
    PipelineDesc p;
    p.layout = vlayout();
    p.cull = CullMode::none;
    p.depth_test = false;
    p.shader.software.vertex = [](const VertexInput& in, const Bindings&, VaryingBlock& out) {
        const Vector4f c = in.attribute(1);
        out.f[0] = c.x, out.f[1] = c.y, out.f[2] = c.z, out.f[3] = c.w;
        const Vector4f pos = in.attribute(0);
        return Vector4f{pos.x, pos.y, pos.z, 1.0f};
    };
    p.shader.software.fragment = [use_tint](const FragmentInput& f) -> std::optional<ColorF> {
        ColorF c{f.v.f[0], f.v.f[1], f.v.f[2], f.v.f[3]};
        if (use_tint) {
            const Tint t = f.bindings.uniform<Tint>(0);
            c = {c.r * t.r, c.g * t.g, c.b * t.b, c.a * t.a};
        }
        return c;
    };
    const std::string h = glsl_header(d);
    p.shader.glsl_vertex = h + "layout(location=0) in vec3 p; layout(location=1) in vec4 c; out vec4 vc;\nvoid main(){ vc=c; gl_Position=vec4(p,1.0); }\n";
    p.shader.glsl_fragment = h + std::string("in vec4 vc; out vec4 o;\n") + (use_tint ? "layout(std140) uniform U { vec4 tint; };\nvoid main(){ o = vc*tint; }\n" : "void main(){ o = vc; }\n");
    if (use_tint) p.shader.uniform_blocks = {"U"};
    return p;
}

/// Samples texture slot 0 with the interpolated uv (varying 0/1 carry uv); vertex colour attribute 1 holds uv in r,g.
PipelineDesc texture_pipeline(const Device& d) {
    PipelineDesc p = color_pipeline(d, false);
    p.shader.software.fragment = [](const FragmentInput& f) -> std::optional<ColorF> {
        return f.bindings.sample(0, {f.v.f[0], f.v.f[1]}, 0.0f);
    };
    const std::string h = glsl_header(d);
    p.shader.glsl_fragment = h + "in vec4 vc; out vec4 o; uniform sampler2D tex;\nvoid main(){ o = texture(tex, vc.xy); }\n";
    p.shader.samplers = {"tex"};
    return p;
}

Color px(const Framebuffer& fb, usize x, usize y) { return fb.color()->read()(x, y); }
bool near(Color a, Color b, int tol) {
    return std::abs(int(a.r) - int(b.r)) <= tol && std::abs(int(a.g) - int(b.g)) <= tol && std::abs(int(a.b) - int(b.b)) <= tol && std::abs(int(a.a) - int(b.a)) <= tol;
}

std::vector<Backend> backends() {
    std::vector<Backend> r{Backend::software};
#if defined(FLYG_GPU_OPENGL)
    try {
        auto d = create_device(Backend::opengl);
        r.push_back(Backend::opengl);
    } catch (const GraphicsException& e) {
        std::printf("    [opengl backend unavailable: %s]\n", e.what());
    }
#endif
    return r;
}

// full-screen-ish triangle covering the lower-left half in clip space; pure red
const V kTri[3] = {{-1, -1, 0, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 0, 1}, {-1, 1, 0, 1, 0, 0, 1}};

}  // namespace

FLYG_TEST(gpu_factory_and_info) {
    auto sw = create_device(Backend::software);
    CHECK(sw->info().backend == Backend::software);
    CHECK(backend_compiled(Backend::software));
#if !defined(FLYG_GPU_OPENGL)
    CHECK(!backend_compiled(Backend::opengl));
    CHECK_THROWS_AS(create_device(Backend::opengl), GraphicsException);
#endif
}

FLYG_TEST(gpu_triangle_orientation_and_clear) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        auto fb = dev->create_render_target(32, 32);
        auto pipe = dev->create_pipeline(color_pipeline(*dev, false));
        auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(kTri));
        CommandList cl;
        cl.begin_pass({fb, Color{0, 0, 255, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).draw(3).end_pass();
        dev->submit(cl);
        dev->finish();
        // clip y up, window origin top-left: the triangle covers the lower-left half of the image
        CHECK(near(px(*fb, 4, 28), Color{255, 0, 0, 255}, 1));
        CHECK(near(px(*fb, 4, 20), Color{255, 0, 0, 255}, 1));
        CHECK(near(px(*fb, 28, 4), Color{0, 0, 255, 255}, 1));
        CHECK(near(px(*fb, 20, 4), Color{0, 0, 255, 255}, 1));
    }
}

FLYG_TEST(gpu_indexed_strip_fan_and_offsets) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        const V quad[5] = {{-1, -1, 0, 0, 1, 0, 1}, {1, -1, 0, 0, 1, 0, 1}, {-1, 1, 0, 0, 1, 0, 1}, {1, 1, 0, 0, 1, 0, 1}, {0, 0, 0, 0, 1, 0, 1}};
        auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(quad));
        const u16 idx[6] = {0, 1, 2, 2, 1, 3};
        auto ib = dev->create_buffer(BufferUsage::index, std::span<const u16>(idx));
        auto run = [&](Topology t, bool indexed) {
            PipelineDesc pd = color_pipeline(*dev, false);
            pd.topology = t;
            auto pipe = dev->create_pipeline(pd);
            auto fb = dev->create_render_target(16, 16);
            CommandList cl;
            cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb);
            if (indexed) cl.set_index_buffer(ib, IndexType::u16).draw_indexed(6);
            else cl.draw(4);
            cl.end_pass();
            dev->submit(cl);
            return fb;
        };
        auto a = run(Topology::triangles, true);
        auto s = run(Topology::triangle_strip, false);
        for (auto& f : {a, s})
            for (usize y : {2u, 13u})
                for (usize x : {2u, 13u}) CHECK(near(px(*f, x, y), Color{0, 255, 0, 255}, 1));
        // fan around vertex 4 (centre): 4,0,1,3,2,0 is not a vertex order we have; use first_index/base_vertex instead
        const u16 idx2[3] = {0, 1, 2};
        auto ib2 = dev->create_buffer(BufferUsage::index, std::span<const u16>(idx2));
        PipelineDesc pd = color_pipeline(*dev, false);
        auto pipe = dev->create_pipeline(pd);
        auto fb = dev->create_render_target(16, 16);
        const V two[6] = {{0, 0, 0, 1, 1, 1, 1}, {0, 0, 0, 1, 1, 1, 1}, {0, 0, 0, 1, 1, 1, 1}, {-1, -1, 0, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 0, 1}, {-1, 1, 0, 1, 0, 0, 1}};
        auto vb2 = dev->create_buffer(BufferUsage::vertex, std::span<const V>(two));
        CommandList cl;
        cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb2).set_index_buffer(ib2, IndexType::u16).draw_indexed(3, 0, 3).end_pass();
        dev->submit(cl);
        CHECK(near(px(*fb, 2, 13), Color{255, 0, 0, 255}, 1));  // base_vertex 3 selects the red triangle
        CHECK(near(px(*fb, 13, 2), Color{0, 0, 0, 255}, 1));
    }
}

FLYG_TEST(gpu_uniforms_update_between_submits) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        auto pipe = dev->create_pipeline(color_pipeline(*dev, true));
        auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(kTri));
        auto ub = dev->create_buffer(BufferDesc{256, BufferUsage::uniform});
        auto fb = dev->create_render_target(16, 16);
        auto draw_with = [&](Tint t) {
            ub->write(0, std::as_bytes(std::span<const Tint>(&t, 1)));
            CommandList cl;
            cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).set_uniform(0, ub, 0, sizeof(Tint)).draw(3).end_pass();
            dev->submit(cl);
        };
        draw_with({1, 1, 1, 1});
        CHECK(near(px(*fb, 2, 13), Color{255, 0, 0, 255}, 1));
        draw_with({0.5f, 1, 1, 1});
        CHECK(near(px(*fb, 2, 13), Color{128, 0, 0, 255}, 2));
        auto back = ub->read(0, sizeof(Tint));
        Tint t{};
        std::memcpy(&t, back.data(), sizeof t);
        CHECK(t.r == 0.5f);
    }
}

FLYG_TEST(gpu_texture_sampling_and_render_to_texture) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        // pass 1: render a 2-colour image into a texture
        auto rt = dev->create_render_target(16, 16, PixelFormat::rgba8_unorm, false);
        auto cpipe = dev->create_pipeline(color_pipeline(*dev, false));
        auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(kTri));
        CommandList c1;
        c1.begin_pass({rt, Color{0, 0, 255, 255}, std::nullopt}).set_pipeline(cpipe).set_vertex_buffer(vb).draw(3).end_pass();
        dev->submit(c1);
        // pass 2: full-screen quad sampling it
        const V quad[4] = {{-1, -1, 0, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 0, 1}, {-1, 1, 0, 0, 0, 0, 1}, {1, 1, 0, 1, 0, 0, 1}};  // uv.y = 0 at the top
        auto qb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(quad));
        PipelineDesc td = texture_pipeline(*dev);
        td.topology = Topology::triangle_strip;
        auto tpipe = dev->create_pipeline(td);
        auto out = dev->create_render_target(16, 16, PixelFormat::rgba8_unorm, false);
        SamplerDesc nearest;
        nearest.min = nearest.mag = FilterMode::nearest;
        nearest.mipmaps = false;
        nearest.wrap_s = nearest.wrap_t = Wrap::clamp;
        CommandList c2;
        c2.begin_pass({out, Color{0, 0, 0, 255}, std::nullopt}).set_pipeline(tpipe).set_vertex_buffer(qb).set_texture(0, rt->color(), nearest).draw(4).end_pass();
        dev->submit(c2);
        CHECK(near(px(*out, 3, 12), Color{255, 0, 0, 255}, 2));
        CHECK(near(px(*out, 12, 3), Color{0, 0, 255, 255}, 2));
    }
}

FLYG_TEST(gpu_uploaded_texture_and_srgb) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        Image img(2, 2);
        img(0, 0) = Color{255, 0, 0, 255}; img(1, 0) = Color{0, 255, 0, 255};
        img(0, 1) = Color{0, 0, 255, 255}; img(1, 1) = Color{255, 255, 255, 255};
        auto tex = dev->create_texture(img, PixelFormat::rgba8_unorm);
        CHECK(tex->read()(1, 0).g == 255);
        const V quad[4] = {{-1, -1, 0, 0, 1, 0, 1}, {1, -1, 0, 1, 1, 0, 1}, {-1, 1, 0, 0, 0, 0, 1}, {1, 1, 0, 1, 0, 0, 1}};
        auto qb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(quad));
        PipelineDesc td = texture_pipeline(*dev);
        td.topology = Topology::triangle_strip;
        auto pipe = dev->create_pipeline(td);
        SamplerDesc nearest;
        nearest.min = nearest.mag = FilterMode::nearest;
        nearest.mipmaps = false;
        nearest.wrap_s = nearest.wrap_t = Wrap::clamp;
        auto fb = dev->create_render_target(8, 8);
        CommandList cl;
        cl.begin_pass({fb, Color{0, 0, 0, 255}, std::nullopt}).set_pipeline(pipe).set_vertex_buffer(qb).set_texture(0, tex, nearest).draw(4).end_pass();
        dev->submit(cl);
        CHECK(near(px(*fb, 1, 1), Color{255, 0, 0, 255}, 1));
        CHECK(near(px(*fb, 6, 1), Color{0, 255, 0, 255}, 1));
        CHECK(near(px(*fb, 1, 6), Color{0, 0, 255, 255}, 1));
        CHECK(near(px(*fb, 6, 6), Color{255, 255, 255, 255}, 1));
        // r8 texture round trip
        auto r8 = dev->create_texture(TextureDesc{2, 1, PixelFormat::r8_unorm, false});
        const std::byte rb[2] = {std::byte{10}, std::byte{200}};
        r8->write(std::span<const std::byte>(rb));
        CHECK_EQ(int(r8->read()(1, 0).r), 200);
        // srgb render target: mid grey written as 0.5 is stored encoded (~188)
        auto sfb = dev->create_render_target(4, 4, PixelFormat::rgba8_srgb, false);
        PipelineDesc cd = color_pipeline(*dev, false);
        auto cp = dev->create_pipeline(cd);
        const V grey[3] = {{-1, -1, 0, .5f, .5f, .5f, 1}, {1, -1, 0, .5f, .5f, .5f, 1}, {-1, 1, 0, .5f, .5f, .5f, 1}};
        auto gb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(grey));
        CommandList cs;
        cs.begin_pass({sfb, Color{0, 0, 0, 255}, std::nullopt}).set_pipeline(cp).set_vertex_buffer(gb).draw(3).end_pass();
        dev->submit(cs);
        CHECK(near(px(*sfb, 0, 3), Color{188, 188, 188, 255}, 3));
    }
}

FLYG_TEST(gpu_viewport_depth_blend_cull) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        // viewport restricts drawing to the right half
        {
            auto pipe = dev->create_pipeline(color_pipeline(*dev, false));
            const V full[4] = {{-1, -1, 0, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 0, 1}, {-1, 1, 0, 1, 0, 0, 1}, {1, 1, 0, 1, 0, 0, 1}};
            PipelineDesc pd = color_pipeline(*dev, false);
            pd.topology = Topology::triangle_strip;
            pipe = dev->create_pipeline(pd);
            auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(full));
            auto fb = dev->create_render_target(16, 16);
            CommandList cl;
            cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).set_viewport(8, 0, 8, 16).draw(4).end_pass();
            dev->submit(cl);
            CHECK(near(px(*fb, 3, 8), Color{0, 0, 0, 255}, 1));
            CHECK(near(px(*fb, 12, 8), Color{255, 0, 0, 255}, 1));
        }
        // depth test: near green quad drawn first must survive a far red quad
        {
            PipelineDesc pd = color_pipeline(*dev, false);
            pd.depth_test = true;
            pd.topology = Topology::triangle_strip;
            auto pipe = dev->create_pipeline(pd);
            const V nearq[4] = {{-1, -1, -0.5f, 0, 1, 0, 1}, {1, -1, -0.5f, 0, 1, 0, 1}, {-1, 1, -0.5f, 0, 1, 0, 1}, {1, 1, -0.5f, 0, 1, 0, 1}};
            const V farq[4] = {{-1, -1, 0.5f, 1, 0, 0, 1}, {1, -1, 0.5f, 1, 0, 0, 1}, {-1, 1, 0.5f, 1, 0, 0, 1}, {1, 1, 0.5f, 1, 0, 0, 1}};
            auto nb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(nearq));
            auto fbuf = dev->create_buffer(BufferUsage::vertex, std::span<const V>(farq));
            auto fb = dev->create_render_target(8, 8);
            CommandList cl;
            cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(nb).draw(4).set_vertex_buffer(fbuf).draw(4).end_pass();
            dev->submit(cl);
            CHECK(near(px(*fb, 4, 4), Color{0, 255, 0, 255}, 1));
        }
        // alpha blending: 50% white over black
        {
            PipelineDesc pd = color_pipeline(*dev, false);
            pd.blend = true;
            auto pipe = dev->create_pipeline(pd);
            const V t[3] = {{-1, -1, 0, 1, 1, 1, 0.5f}, {1, -1, 0, 1, 1, 1, 0.5f}, {-1, 1, 0, 1, 1, 1, 0.5f}};
            auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(t));
            auto fb = dev->create_render_target(8, 8);
            CommandList cl;
            cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).draw(3).end_pass();
            dev->submit(cl);
            CHECK(near(px(*fb, 1, 6), Color{128, 128, 128, 255}, 3));
        }
        // back-face culling: clockwise triangle disappears with front_ccw = true
        {
            PipelineDesc pd = color_pipeline(*dev, false);
            pd.cull = CullMode::back;
            auto pipe = dev->create_pipeline(pd);
            const V cw[3] = {{-1, -1, 0, 1, 0, 0, 1}, {-1, 1, 0, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 0, 1}};
            auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(cw));
            auto fb = dev->create_render_target(8, 8);
            CommandList cl;
            cl.begin_pass({fb, Color{0, 0, 0, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).draw(3).end_pass();
            dev->submit(cl);
            CHECK(near(px(*fb, 1, 6), Color{0, 0, 0, 255}, 1));
        }
    }
}

FLYG_TEST(gpu_error_cases) {
    for (Backend b : backends()) {
        auto dev = create_device(b);
        CommandList cl;
        CHECK_THROWS_AS(cl.draw(3), GraphicsException);
        CHECK_THROWS_AS(cl.end_pass(), GraphicsException);
        CHECK_THROWS_AS(dev->create_buffer(BufferDesc{0, BufferUsage::vertex}), GraphicsException);
        CHECK_THROWS_AS(dev->create_texture(TextureDesc{0, 4, PixelFormat::rgba8_unorm, false}), GraphicsException);
        auto buf = dev->create_buffer(BufferDesc{16, BufferUsage::vertex});
        CHECK_THROWS_AS(buf->write(8, std::vector<std::byte>(16)), GraphicsException);
        CHECK_THROWS_AS(buf->read(0, 17), GraphicsException);
        auto plain = dev->create_texture(TextureDesc{4, 4, PixelFormat::rgba8_unorm, false});
        CHECK_THROWS_AS(dev->create_framebuffer(plain, false), GraphicsException);
        auto fb = dev->create_render_target(4, 4);
        CommandList open;
        open.begin_pass({fb});
        CHECK_THROWS_AS(dev->submit(open), GraphicsException);
        CommandList nodraw;
        nodraw.begin_pass({fb}).draw(3).end_pass();
        CHECK_THROWS_AS(dev->submit(nodraw), GraphicsException);
        PipelineDesc bad = color_pipeline(*dev, false);
        bad.layout.attributes.push_back({2, VertexFormat::float4, 100});
        CHECK_THROWS_AS(dev->create_pipeline(bad), GraphicsException);
    }
#if defined(FLYG_GPU_OPENGL)
    if (backends().size() > 1) {
        auto dev = create_device(Backend::opengl);
        PipelineDesc p = color_pipeline(*dev, false);
        p.shader.glsl_fragment = glsl_header(*dev) + "void main(){ this is not glsl }";
        CHECK_THROWS_AS(dev->create_pipeline(p), GraphicsException);
    }
#endif
}

FLYG_TEST(gpu_backends_agree_on_a_scene) {
    const auto bs = backends();
    if (bs.size() < 2) return;  // only meaningful when OpenGL is available
    std::vector<Image> imgs;
    for (Backend b : bs) {
        auto dev = create_device(b);
        PipelineDesc pd = color_pipeline(*dev, false);
        pd.depth_test = true;
        pd.cull = CullMode::back;
        auto pipe = dev->create_pipeline(pd);
        const V s[6] = {{-0.9f, -0.8f, 0.2f, 1, 0, 0, 1}, {0.8f, -0.7f, 0.2f, 0, 1, 0, 1}, {0.1f, 0.9f, 0.2f, 0, 0, 1, 1},
                        {-0.5f, 0.5f, -0.3f, 1, 1, 0, 1}, {-0.4f, -0.9f, -0.3f, 0, 1, 1, 1}, {0.9f, 0.2f, -0.3f, 1, 0, 1, 1}};
        auto vb = dev->create_buffer(BufferUsage::vertex, std::span<const V>(s));
        auto fb = dev->create_render_target(64, 64);
        CommandList cl;
        cl.begin_pass({fb, Color{20, 30, 40, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).draw(6).end_pass();
        dev->submit(cl);
        imgs.push_back(fb->color()->read());
    }
    usize bad = 0, total = 0;
    int worst = 0;
    for (usize y = 0; y < 64; ++y)
        for (usize x = 0; x < 64; ++x) {
            const Color a = imgs[0](x, y), c = imgs[1](x, y);
            const int d = std::max({std::abs(int(a.r) - int(c.r)), std::abs(int(a.g) - int(c.g)), std::abs(int(a.b) - int(c.b))});
            worst = std::max(worst, d);
            ++total;
            if (d > 6) ++bad;  // edge pixels may differ by rasterisation rules
        }
    std::printf("    [software vs opengl: %zu/%zu pixels differ by >6, worst %d]\n", static_cast<size_t>(bad), static_cast<size_t>(total), worst);
    CHECK(bad * 100 < total);  // < 1 % (triangle edges only)
}
