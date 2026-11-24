// flyG - graphics module tests: meshes, OBJ loader, textures and the software 3D renderer
// SPDX-License-Identifier: GPL-2.0-only
#include <flyg/graphics/graphics.hpp>

#include "test_framework.hpp"

using namespace flyg;
using namespace flyg::graphics;
namespace fmath = flyg::math;

namespace {
/// Outputs a constant colour (already encoded; use with linear_output = false).
struct ConstShader {
    using Varying = StandardVarying;
    Matrix4f mvp = Matrix4f::identity();
    ColorF color{1, 1, 1, 1};
    Vector4f vertex(const Vertex& v, Varying& out) const {
        out = {};
        out.uv = v.uv;
        out.world_pos = v.position;
        return mvp * Vector4f(v.position, 1.0f);
    }
    std::optional<ColorF> fragment(const Varying&) const { return color; }
};
/// Outputs interpolated uv as colour.
struct UvShader {
    using Varying = StandardVarying;
    Matrix4f mvp = Matrix4f::identity();
    Vector4f vertex(const Vertex& v, Varying& out) const {
        out = {};
        out.uv = v.uv;
        return mvp * Vector4f(v.position, 1.0f);
    }
    std::optional<ColorF> fragment(const Varying& in) const { return ColorF{in.uv.x, in.uv.y, 0, 1}; }
};
DrawState raw() {
    DrawState s;
    s.linear_output = false;
    return s;
}
Mesh triangle(Vector3f a, Vector3f b, Vector3f c) {
    Mesh m;
    m.vertices = {{a, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}}, {b, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}}, {c, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}}};
    m.indices = {0, 1, 2};
    return m;
}
usize count_alpha(const Image& im) {
    usize n = 0;
    for (const Color& c : im.pixels()) n += c.a != 0;
    return n;
}
}  // namespace

FLYG_TEST(graphics_mesh_primitives) {
    const Mesh b = primitives::box(2, 4, 6);
    CHECK_EQ(b.triangle_count(), 12u);
    CHECK_NOTHROW(b.validate());
    const Bounds3 bb = b.bounds();
    CHECK_EQ(bb.size().y, 4.0f);
    for (const Vertex& v : b.vertices) CHECK(std::fabs(fmath::length(v.normal) - 1) < 1e-5f);
    // outward winding: every triangle normal points away from the centre
    for (usize t = 0; t < b.indices.size(); t += 3) {
        const Vector3f a = b.vertices[b.indices[t]].position, c = b.vertices[b.indices[t + 1]].position, d = b.vertices[b.indices[t + 2]].position;
        const Vector3f n = fmath::cross(c - a, d - a);
        CHECK(fmath::dot(n, a) > 0);
    }
    const Mesh s = primitives::sphere(2.0f, 64, 32);
    double area = 0, vol = 0;
    for (usize t = 0; t < s.indices.size(); t += 3) {
        const Vector3f a = s.vertices[s.indices[t]].position, c = s.vertices[s.indices[t + 1]].position, d = s.vertices[s.indices[t + 2]].position;
        const Vector3f n = fmath::cross(c - a, d - a);
        area += fmath::length(n) / 2;
        vol += fmath::dot(a, n) / 6;
    }
    CHECK(std::fabs(area - 4 * 3.14159265 * 4) < 0.01 * 4 * 3.14159265 * 4);
    CHECK(vol > 0 && std::fabs(vol - 4.0 / 3 * 3.14159265 * 8) < 0.02 * 33.5);
    for (const Mesh& m : {primitives::cylinder(), primitives::cone(), primitives::torus(), primitives::plane(2, 2, 3, 4)}) {
        CHECK(!m.empty());
        CHECK_NOTHROW(m.validate());
    }
    CHECK_EQ(primitives::plane(1, 1, 3, 4).triangle_count(), 24u);
    CHECK_THROWS_AS(primitives::sphere(1, 2, 8), GraphicsException);
    CHECK_THROWS_AS(primitives::plane(1, 1, 0, 1), GraphicsException);
    Mesh bad = triangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    bad.indices[2] = 9;
    CHECK_THROWS_AS(bad.validate(), GraphicsException);
    // smooth normals of a flat triangle; flat conversion duplicates vertices
    Mesh t = triangle({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    t.compute_normals();
    CHECK(std::fabs(t.vertices[0].normal.z - 1) < 1e-6f);
    Mesh bx = primitives::box();
    bx.make_flat();
    CHECK_EQ(bx.vertices.size(), 36u);
    bx.normalise_size(10.0f);
    CHECK(std::fabs(bx.bounds().size().x - 10.0f) < 1e-4f);
}

FLYG_TEST(graphics_obj_loader) {
    const char* text =
        "# comment\nmtllib scene.mtl\no cube\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1 \n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
        "usemtl red\nf 1/1/1 2/2/1 3/3/1 4/4/1\n"
        "usemtl blue\nf -1 -2 -3\n";
    const obj::Model m = obj::parse(text);
    CHECK_EQ(m.mesh.triangle_count(), 3u);  // quad -> 2 triangles + 1
    CHECK_EQ(m.material_libraries.size(), 1u);
    CHECK_EQ(m.mesh.groups.size(), 2u);
    CHECK(m.mesh.groups[0].material == "red");
    CHECK_EQ(m.mesh.groups[0].index_count, 6u);
    CHECK_EQ(m.mesh.groups[1].first_index, 6u);
    CHECK_EQ(m.mesh.vertices[1].uv.x, 1.0f);
    CHECK_NOTHROW(m.mesh.validate());
    CHECK_THROWS_AS(obj::parse("v 0 0 0\nf 1 2 3\n"), DataException);          // index out of range
    CHECK_THROWS_AS(obj::parse("v 0 0\n"), DataException);
    CHECK_THROWS_AS(obj::parse("v 0 0 x\nf 1 1 1\n"), DataException);
    CHECK_THROWS_AS(obj::parse("v 0 0 0\n"), DataException);                    // no faces
    const auto mats = obj::parse_mtl("newmtl red\nKd 1 0 0\nKs 0.5 0.5 0.5\nNs 10\nd 0.5\nmap_Kd tex/red.png\nnewmtl b\nTr 0.25\n");
    CHECK_EQ(mats.size(), 2u);
    CHECK_EQ(mats[0].diffuse.x, 1.0f);
    CHECK_EQ(mats[0].opacity, 0.5f);
    CHECK(mats[0].diffuse_map == "tex/red.png");
    CHECK_EQ(mats[1].opacity, 0.75f);
    CHECK_THROWS_AS(obj::load("/nonexistent/x.obj"), DataException);
}

FLYG_TEST(graphics_texture_sampling) {
    Image im(2, 2);
    im(0, 0) = Color{255, 0, 0}; im(1, 0) = Color{0, 255, 0}; im(0, 1) = Color{0, 0, 255}; im(1, 1) = Color{255, 255, 255};
    Texture t(im, true, true);
    CHECK_EQ(t.level_count(), 2u);
    t.filter = TexFilter::nearest;
    CHECK_EQ(t.sample({0.25f, 0.25f}).r, 1.0f);
    CHECK_EQ(t.sample({0.75f, 0.25f}).g, 1.0f);
    CHECK_EQ(t.sample({1.25f, 0.25f}).r, 1.0f);   // repeat
    t.wrap_s = Wrap::clamp;
    CHECK_EQ(t.sample({1.25f, 0.25f}).g, 1.0f);
    t.wrap_s = Wrap::mirror;
    CHECK_EQ(t.sample({1.25f, 0.25f}).g, 1.0f);
    t.filter = TexFilter::bilinear;
    t.wrap_s = t.wrap_t = Wrap::clamp;
    const ColorF mid = t.sample({0.5f, 0.5f});
    CHECK(std::fabs(mid.r - 0.5f * (1 + 0) / 2 - 0.25f * 0) < 0.35f);
    // sRGB: 128 -> ~0.2158 linear; data textures stay raw
    Image g(1, 1, Color{128, 128, 128, 255});
    CHECK(std::fabs(Texture(g, false, true).sample({0.5f, 0.5f}).r - 0.2158f) < 0.002f);
    CHECK(std::fabs(Texture(g, false, false).sample({0.5f, 0.5f}).r - 128.0f / 255.0f) < 0.002f);
    // trilinear at a high LOD approaches the average colour
    Texture m(im, true, false);
    const ColorF avg = m.sample({0.5f, 0.5f}, 5.0f);
    CHECK(std::fabs(avg.r - 0.5f) < 0.01f && std::fabs(avg.g - 0.5f) < 0.01f);
    CHECK_THROWS_AS(Texture(Image{}), GraphicsException);
}

FLYG_TEST(graphics_renderer_coverage_and_shared_edges) {
    Renderer r(32, 32);
    r.clear(Color{0, 0, 0, 0});
    ConstShader sh;
    sh.color = ColorF{1, 0, 0, 0.5f};
    DrawState st = raw();
    st.blend = true;
    st.depth_test = false;
    st.cull = CullMode::none;
    // a quad from two triangles sharing a diagonal: every pixel must be hit exactly once
    const Mesh a = triangle({-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0.5f, 0.5f, 0});
    const Mesh b = triangle({-0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}, {-0.5f, 0.5f, 0});
    r.draw(a, sh, st);
    r.draw(b, sh, st);
    const Image im = r.resolve();
    CHECK_EQ(count_alpha(im), 16u * 16u);
    for (usize y = 8; y < 24; ++y)
        for (usize x = 8; x < 24; ++x) CHECK_EQ(im(x, y).a, 128);   // never 192 (double hit)
    CHECK_EQ(im(7, 16).a, 0);
    CHECK_EQ(r.stats().triangles_rasterised, 2u);
}

FLYG_TEST(graphics_renderer_perspective_correct_interpolation) {
    const usize W = 64, H = 64;
    Renderer r(W, H);
    r.clear(Color{0, 0, 0, 255});
    Camera cam;
    cam.eye = {0, 2, 3};
    cam.target = {0, 0, 0};
    cam.fov_y = 1.0f;
    UvShader sh;
    sh.mvp = cam.view_projection(r.aspect());
    r.draw(primitives::plane(4, 4, 1, 1), sh, [] { DrawState s = raw(); s.cull = CullMode::none; return s; }());
    const Image im = r.resolve();
    // analytic ray / plane (y = 0) intersection for several pixels
    const Matrix4f inv = fmath::inverse(sh.mvp);
    int checked = 0;
    for (usize py = 30; py < H; py += 7)
        for (usize px = 10; px < W; px += 11) {
            const float nx = (static_cast<float>(px) + 0.5f) / W * 2 - 1, ny = 1 - (static_cast<float>(py) + 0.5f) / H * 2;
            Vector4f n = inv * Vector4f{nx, ny, -1, 1}, f = inv * Vector4f{nx, ny, 1, 1};
            const Vector3f pn = n.xyz() * (1 / n.w), pf = f.xyz() * (1 / f.w);
            const float t = pn.y / (pn.y - pf.y);
            const Vector3f hit = pn + (pf - pn) * t;
            if (std::fabs(hit.x) >= 1.95f || std::fabs(hit.z) >= 1.95f) continue;
            const float u = hit.x / 4 + 0.5f, v = hit.z / 4 + 0.5f;
            CHECK(std::abs(int(im(px, py).r) - int(u * 255 + 0.5f)) <= 3);
            CHECK(std::abs(int(im(px, py).g) - int(v * 255 + 0.5f)) <= 3);
            ++checked;
        }
    CHECK(checked > 15);
}

FLYG_TEST(graphics_renderer_depth_culling_clipping) {
    Renderer r(32, 32);
    ConstShader red, blue;
    red.color = ColorF{1, 0, 0, 1};
    blue.color = ColorF{0, 0, 1, 1};
    const Mesh near_t = triangle({-0.8f, -0.8f, -0.5f}, {0.8f, -0.8f, -0.5f}, {0, 0.8f, -0.5f});
    const Mesh far_t = triangle({-0.8f, -0.8f, 0.5f}, {0.8f, -0.8f, 0.5f}, {0, 0.8f, 0.5f});
    for (int order = 0; order < 2; ++order) {
        r.clear(Color{0, 0, 0, 255});
        if (order == 0) { r.draw(near_t, red, raw()); r.draw(far_t, blue, raw()); }
        else { r.draw(far_t, blue, raw()); r.draw(near_t, red, raw()); }
        CHECK_EQ(r.resolve()(16, 20).r, 255);  // nearer triangle wins independent of draw order
        CHECK_EQ(r.resolve()(16, 20).b, 0);
    }
    // depth functions
    r.clear(Color{0, 0, 0, 255});
    r.draw(near_t, red, raw());
    DrawState gr = raw();
    gr.depth_func = DepthFunc::lequal;
    r.draw(far_t, blue, gr);
    CHECK_EQ(r.resolve()(16, 20).b, 0);            // far fails "lequal" against near
    gr.depth_func = DepthFunc::greater;
    r.draw(far_t, blue, gr);
    CHECK_EQ(r.resolve()(16, 20).b, 255);          // far is "greater" than near
    r.clear(Color{0, 0, 0, 255});
    r.draw(near_t, red, raw());
    gr.depth_func = DepthFunc::always;
    r.draw(far_t, blue, gr);
    CHECK_EQ(r.resolve()(16, 20).b, 255);
    // culling: CCW triangle is front facing; reversed is culled; none draws both; front_ccw flips
    const Mesh ccw = triangle({-0.8f, -0.8f, 0}, {0.8f, -0.8f, 0}, {0, 0.8f, 0});
    const Mesh cw = triangle({-0.8f, -0.8f, 0}, {0, 0.8f, 0}, {0.8f, -0.8f, 0});
    auto drawn = [&](const Mesh& m, DrawState s) { r.clear(Color{0, 0, 0, 0}); r.draw(m, red, s); return count_alpha(r.resolve()) > 0; };
    CHECK(drawn(ccw, raw()));
    CHECK(!drawn(cw, raw()));
    DrawState none = raw();
    none.cull = CullMode::none;
    CHECK(drawn(cw, none));
    DrawState cf = raw();
    cf.cull = CullMode::front;
    CHECK(!drawn(ccw, cf));
    CHECK(drawn(cw, cf));
    DrawState cw_front = raw();
    cw_front.front_ccw = false;
    CHECK(drawn(cw, cw_front));
    // near-plane clipping: a huge floor crossing behind the camera must not throw or smear
    Camera cam;
    cam.eye = {0, 1, 0};
    cam.target = {0, 1, -5};
    r.clear(Color{0, 0, 0, 0});
    ConstShader floor;
    floor.mvp = cam.view_projection(r.aspect());
    DrawState nc = raw();
    nc.cull = CullMode::none;
    r.draw(primitives::plane(100, 100, 4, 4), floor, nc);
    const Image im = r.resolve();
    CHECK(im(16, 31).a == 255);   // floor visible below the horizon
    CHECK(im(16, 2).a == 0);      // sky above stays empty
    // non-finite and out-of-range input
    Mesh nan = triangle({std::nanf(""), 0, 0}, {1, 0, 0}, {0, 1, 0});
    CHECK_NOTHROW(r.draw(nan, red, raw()));
    CHECK_THROWS_AS(Renderer(0, 4), GraphicsException);
    CHECK_THROWS_AS(Renderer(4, 4, 9), GraphicsException);
    std::vector<u32> idx = {0, 1, 7};
    CHECK_THROWS_AS(r.draw(std::span<const Vertex>(near_t.vertices), std::span<const u32>(idx), red, raw()), GraphicsException);
}

FLYG_TEST(graphics_renderer_shading_aa_and_modes) {
    // supersampling gives intermediate edge values and the same interior
    Renderer r1(32, 32, 1), r3(32, 32, 3);
    ConstShader sh;
    sh.color = ColorF{1, 1, 1, 1};
    const Mesh t = triangle({-0.9f, -0.9f, 0}, {0.9f, -0.5f, 0}, {-0.2f, 0.9f, 0});
    r1.clear(Color{0, 0, 0, 255}), r3.clear(Color{0, 0, 0, 255});
    r1.draw(t, sh, raw()), r3.draw(t, sh, raw());
    const Image a = r1.resolve(), b = r3.resolve();
    int soft = 0;
    for (const Color& c : b.pixels()) soft += c.r > 10 && c.r < 245;
    int hard = 0;
    for (const Color& c : a.pixels()) hard += c.r > 10 && c.r < 245;
    CHECK(soft > 20);
    CHECK_EQ(hard, 0);
    CHECK_EQ(b(10, 20).r, 255);

    // Blinn-Phong: a surface facing the light is brighter than one facing away; the specular term adds light
    Camera cam;
    cam.eye = {0, 0, 4};
    Renderer rr(32, 32);
    rr.clear(Color{0, 0, 0, 255});
    BlinnPhongShader bp;
    bp.set_camera(cam, 1.0f);
    bp.ambient_light = {0, 0, 0};
    Light l;
    l.direction = {0, 0, -1};  // travels towards -z, lighting the +z face
    bp.lights = {l};
    bp.material.specular = {0, 0, 0};
    rr.draw(primitives::sphere(1.0f, 32, 16), bp);
    const Image lit = rr.resolve();
    CHECK(lit(16, 16).r > 200);             // facing the light
    CHECK(lit(16, 16).r > lit(24, 16).r);   // grazing area is darker
    bp.lights[0].direction = {0, 0, 1};     // light from behind the sphere
    rr.clear(Color{0, 0, 0, 255});
    rr.draw(primitives::sphere(1.0f, 32, 16), bp);
    CHECK(rr.resolve()(16, 16).r < 5);

    // textured unlit quad with alpha cut-out
    Image tx(2, 2, Color{255, 255, 255, 255});
    tx(0, 0) = Color{255, 255, 255, 0};
    Texture tex(tx, false);
    tex.filter = TexFilter::nearest;
    UnlitShader un;
    un.texture = &tex;
    un.alpha_cutoff = 0.5f;
    Renderer ru(16, 16);
    ru.clear(Color{0, 0, 0, 0});
    Mesh quad = primitives::plane(2, 2, 1, 1);
    quad.flip_winding();  // plane faces +Y; look at it from above
    Camera top;
    top.eye = {0, 2, 0};
    top.target = {0, 0, 0};
    top.up = {0, 0, -1};
    top.orthographic = true;
    top.ortho_height = 2;
    un.view_projection = top.view_projection(1.0f);
    DrawState both = DrawState{};
    both.cull = CullMode::none;
    ru.draw(quad, un, both);
    const Image ui = ru.resolve();
    CHECK_EQ(count_alpha(ui) > 0, true);
    CHECK(count_alpha(ui) < 16 * 16);       // one texel quadrant discarded
    // wireframe and point modes produce sparse output
    Renderer rw(32, 32);
    rw.clear(Color{0, 0, 0, 0});
    DrawState wf = raw();
    wf.polygon = PolygonMode::line;
    rw.draw(t, sh, wf);
    const usize line_px = count_alpha(rw.resolve());
    CHECK(line_px > 20 && line_px < 200);
    rw.clear(Color{0, 0, 0, 0});
    wf.polygon = PolygonMode::point;
    rw.draw(t, sh, wf);
    CHECK_EQ(count_alpha(rw.resolve()), 3u);
}
