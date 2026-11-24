// SPDX-License-Identifier: GPL-2.0-only
// flyG example: 2D canvas, image codecs and the GPU abstraction (software backend; OpenGL when available)
#include <cstdio>

#include <flyg/graphics/graphics.hpp>

using namespace flyg::graphics;

int main() {
    // --- 2D canvas -> PNG in memory -> decode again
    Canvas canvas(128, 96, Color{255, 255, 255, 255});
    canvas.set_fill(linear_gradient({0, 0}, {128, 0}, Gradient{{0, Color{255, 80, 0}}, {1, Color{0, 80, 255}}}));
    canvas.fill_rect(8, 8, 112, 40);
    canvas.set_stroke(Color{0, 0, 0});
    canvas.set_line_width(4);
    canvas.stroke(Path().circle(64, 70, 18));
    const auto png = png::encode(canvas.image());
    const Image back = png::decode(png);
    std::printf("canvas: %zux%zu, png %zu bytes, round trip %s\n", back.width(), back.height(), png.size(), back.width() == 128 ? "ok" : "FAILED");

    // --- GPU abstraction: one coloured triangle
    struct V { float x, y, z, r, g, b, a; };
    auto dev = gpu::create_device(gpu::Backend::software);
    gpu::PipelineDesc pd;
    pd.layout = {sizeof(V), {{0, gpu::VertexFormat::float3, 0}, {1, gpu::VertexFormat::float4, 12}}};
    pd.cull = CullMode::none;
    pd.depth_test = false;
    pd.shader.software.vertex = [](const gpu::VertexInput& in, const gpu::Bindings&, gpu::VaryingBlock& out) {
        const auto c = in.attribute(1);
        out.f[0] = c.x, out.f[1] = c.y, out.f[2] = c.z, out.f[3] = c.w;
        const auto p = in.attribute(0);
        return Vector4f{p.x, p.y, p.z, 1.0f};
    };
    pd.shader.software.fragment = [](const gpu::FragmentInput& f) -> std::optional<ColorF> { return ColorF{f.v.f[0], f.v.f[1], f.v.f[2], f.v.f[3]}; };
    auto pipe = dev->create_pipeline(pd);
    const V tri[3] = {{-0.8f, -0.8f, 0, 1, 0, 0, 1}, {0.8f, -0.8f, 0, 0, 1, 0, 1}, {0, 0.8f, 0, 0, 0, 1, 1}};
    auto vb = dev->create_buffer(gpu::BufferUsage::vertex, std::span<const V>(tri));
    auto target = dev->create_render_target(64, 64);
    gpu::CommandList cl;
    cl.begin_pass({target, Color{16, 16, 16, 255}, 1.0f}).set_pipeline(pipe).set_vertex_buffer(vb).draw(3).end_pass();
    dev->submit(cl);
    const Color centre = target->color()->read()(32, 40);
    std::printf("gpu (%s): centre pixel = (%d,%d,%d)\n", dev->info().name.c_str(), centre.r, centre.g, centre.b);
    return centre.r + centre.g + centre.b > 100 ? 0 : 1;
}
