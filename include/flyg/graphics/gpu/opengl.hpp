// flyG - graphics module: OpenGL backend (optional; define FLYG_GPU_OPENGL, CMake option FLYG_GPU_OPENGL).
// No GL headers or link-time libraries are needed: the driver is loaded at run time
// (Linux: EGL surfaceless/pbuffer-less context via libEGL.so.1, desktop GL 3.3+ core preferred, GLES 3.0 fallback;
//  Windows: WGL context on a hidden window). The Windows path is compile-checked only (not run in CI).
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#if defined(FLYG_GPU_OPENGL)

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  ifdef DrawState  // winuser.h macro that would rename flyg::graphics::DrawState
#    undef DrawState
#  endif
#else
#include <dlfcn.h>
#endif

#include <flyg/graphics/gpu/gpu.hpp>

namespace flyg::graphics::gpu {

namespace gl_detail {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

#if defined(_WIN32) && !defined(_WIN64)
#define FLYG_GLCALL __stdcall
#else
#define FLYG_GLCALL
#endif

enum : unsigned {
    ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, UNIFORM_BUFFER = 0x8A11, DYNAMIC_DRAW = 0x88E8,
    VERTEX_SHADER = 0x8B31, FRAGMENT_SHADER = 0x8B30, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82, INFO_LOG_LENGTH = 0x8B84,
    TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, RGBA = 0x1908, RED = 0x1903, UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406,
    RGBA8 = 0x8058, R8 = 0x8229, SRGB8_ALPHA8 = 0x8C43,
    TEXTURE_MIN_FILTER = 0x2801, TEXTURE_MAG_FILTER = 0x2800, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803,
    NEAREST = 0x2600, LINEAR = 0x2601, NEAREST_MIPMAP_NEAREST = 0x2700, LINEAR_MIPMAP_NEAREST = 0x2701, NEAREST_MIPMAP_LINEAR = 0x2702, LINEAR_MIPMAP_LINEAR = 0x2703,
    REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F, MIRRORED_REPEAT = 0x8370,
    FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00, RENDERBUFFER = 0x8D41, DEPTH_COMPONENT24 = 0x81A6, FRAMEBUFFER_COMPLETE = 0x8CD5,
    DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44, BLEND = 0x0BE2, SCISSOR_TEST = 0x0C11, CW = 0x0900, CCW = 0x0901, FRONT = 0x0404, BACK = 0x0405,
    SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303, ONE = 1,
    LESS = 0x201, EQUAL = 0x202, LEQUAL = 0x203, GREATER = 0x204, NOTEQUAL = 0x205, GEQUAL = 0x206, ALWAYS = 0x207, NEVER = 0x200,
    COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x100, TRIANGLES = 4, TRIANGLE_STRIP = 5, TRIANGLE_FAN = 6, UNSIGNED_SHORT = 0x1403, UNSIGNED_INT = 0x1405,
    UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05, FRAMEBUFFER_SRGB = 0x8DB9, VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02,
    FRONT_AND_BACK = 0x408, POINT = 0x1B00, LINE = 0x1B01, FILL = 0x1B02, LOWER_LEFT = 0x8CA1, UPPER_LEFT = 0x8CA2, NEGATIVE_ONE_TO_ONE = 0x935E,
    UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34, MAX_TEXTURE_SIZE = 0x0D33, NO_ERROR_ = 0, INVALID_INDEX = 0xFFFFFFFFu, MAJOR_VERSION = 0x821B,
};

#define FLYG_GL_FUNCS(X)                                                                                                   \
    X(GLenum, GetError, ())                                                                                                \
    X(const unsigned char*, GetString, (GLenum))                                                                           \
    X(void, GetIntegerv, (GLenum, GLint*))                                                                                 \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                                                \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                                                       \
    X(void, BindBuffer, (GLenum, GLuint))                                                                                  \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                                                         \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*))                                                    \
    X(void, GetBufferSubData, (GLenum, GLintptr, GLsizeiptr, void*))                                                       \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))                                               \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                                           \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                                                  \
    X(void, BindVertexArray, (GLuint))                                                                                     \
    X(void, EnableVertexAttribArray, (GLuint))                                                                             \
    X(void, DisableVertexAttribArray, (GLuint))                                                                            \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                                 \
    X(GLuint, CreateShader, (GLenum))                                                                                      \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                                           \
    X(void, CompileShader, (GLuint))                                                                                       \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                                                         \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                        \
    X(void, DeleteShader, (GLuint))                                                                                        \
    X(GLuint, CreateProgram, ())                                                                                           \
    X(void, AttachShader, (GLuint, GLuint))                                                                                \
    X(void, LinkProgram, (GLuint))                                                                                         \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                                                        \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                                                       \
    X(void, UseProgram, (GLuint))                                                                                          \
    X(void, DeleteProgram, (GLuint))                                                                                       \
    X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar*))                                                               \
    X(void, UniformBlockBinding, (GLuint, GLuint, GLuint))                                                                 \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                                                  \
    X(void, Uniform1i, (GLint, GLint))                                                                                     \
    X(void, GenTextures, (GLsizei, GLuint*))                                                                               \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                                                      \
    X(void, BindTexture, (GLenum, GLuint))                                                                                 \
    X(void, ActiveTexture, (GLenum))                                                                                       \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))                      \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                                                        \
    X(void, GenerateMipmap, (GLenum))                                                                                      \
    X(void, PixelStorei, (GLenum, GLint))                                                                                  \
    X(void, GenFramebuffers, (GLsizei, GLuint*))                                                                           \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*))                                                                  \
    X(void, BindFramebuffer, (GLenum, GLuint))                                                                             \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))                                                 \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                                     \
    X(GLenum, CheckFramebufferStatus, (GLenum))                                                                            \
    X(void, GenRenderbuffers, (GLsizei, GLuint*))                                                                          \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint*))                                                                 \
    X(void, BindRenderbuffer, (GLenum, GLuint))                                                                            \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                                       \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                                           \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                                                    \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei))                                                                     \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                                              \
    X(void, ClearDepth, (double))                                                                                          \
    X(void, Clear, (GLbitfield))                                                                                           \
    X(void, Enable, (GLenum))                                                                                              \
    X(void, Disable, (GLenum))                                                                                             \
    X(void, DepthFunc, (GLenum))                                                                                           \
    X(void, DepthMask, (GLboolean))                                                                                        \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                                                       \
    X(void, CullFace, (GLenum))                                                                                            \
    X(void, FrontFace, (GLenum))                                                                                           \
    X(void, BlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))                                                                                \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                                                          \
    X(void, DrawElements, (GLenum, GLsizei, GLenum, const void*))                                                          \
    X(void, Finish, ())

#define FLYG_GL_OPTIONAL(X)                                                                                                \
    X(void, PolygonMode, (GLenum, GLenum))                                                                                 \
    X(void, ClipControl, (GLenum, GLenum))                                                                                 \
    X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint))

struct Api {
#define X(ret, name, args) ret(FLYG_GLCALL* name) args = nullptr;
    FLYG_GL_FUNCS(X)
    FLYG_GL_OPTIONAL(X)
#undef X
};

// ---- context -----------------------------------------------------------------------------------------------------
class Context {
public:
    Context() {
        open();
        make_current();
        load();
    }
    ~Context() { close(); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Api gl;
    bool desktop = true;
    bool clip_control = false;   ///< window origin is the upper-left corner (images stay top-first)
    int uniform_alignment = 256;

    void make_current() const {
#if defined(_WIN32)
        if (!wglMakeCurrent_(dc_, rc_)) throw GraphicsException("opengl: wglMakeCurrent failed");
#else
        if (!eglMakeCurrent_(dpy_, nullptr, nullptr, ctx_)) throw GraphicsException("opengl: eglMakeCurrent failed");
#endif
    }

private:
    void load() {
        auto get = [&](const char* n) { return proc(n); };
#define X(ret, name, args) gl.name = reinterpret_cast<decltype(gl.name)>(get("gl" #name));
        FLYG_GL_FUNCS(X)
        FLYG_GL_OPTIONAL(X)
#undef X
#define X(ret, name, args) if (!gl.name) throw GraphicsException("opengl: driver lacks gl" #name);
        FLYG_GL_FUNCS(X)
#undef X
        const char* ver = reinterpret_cast<const char*>(gl.GetString(VERSION));
        desktop = ver && std::strncmp(ver, "OpenGL ES", 9) != 0;
        if (!desktop) { gl.PolygonMode = nullptr; gl.DrawElementsBaseVertex = nullptr; }
        if (gl.ClipControl && desktop) { gl.ClipControl(UPPER_LEFT, NEGATIVE_ONE_TO_ONE); clip_control = true; }
        GLint al = 256;
        gl.GetIntegerv(UNIFORM_BUFFER_OFFSET_ALIGNMENT, &al);
        uniform_alignment = al > 0 ? al : 256;
        gl.GetError();
    }

#if defined(_WIN32)
    HWND hwnd_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC rc_ = nullptr;
    HMODULE lib_ = nullptr;
    BOOL (WINAPI* wglMakeCurrent_)(HDC, HGLRC) = nullptr;
    PROC(WINAPI* wglGetProcAddress_)(LPCSTR) = nullptr;
    template <class F>
    F win_fn(const char* n) const { return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(lib_, n))); }
    void open() {
        lib_ = LoadLibraryA("opengl32.dll");
        if (!lib_) throw GraphicsException("opengl: opengl32.dll not found");
        auto wcc = win_fn<HGLRC(WINAPI*)(HDC)>("wglCreateContext");
        wglMakeCurrent_ = win_fn<decltype(wglMakeCurrent_)>("wglMakeCurrent");
        wglGetProcAddress_ = win_fn<decltype(wglGetProcAddress_)>("wglGetProcAddress");
        if (!wcc || !wglMakeCurrent_ || !wglGetProcAddress_) throw GraphicsException("opengl: incomplete opengl32.dll");
        hwnd_ = CreateWindowExA(0, "STATIC", "flyg", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!hwnd_) throw GraphicsException("opengl: cannot create the hidden window");
        dc_ = GetDC(hwnd_);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd), pfd.nVersion = 1, pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL, pfd.iPixelType = PFD_TYPE_RGBA, pfd.cColorBits = 32, pfd.cDepthBits = 24;
        const int pf = ChoosePixelFormat(dc_, &pfd);
        if (!pf || !SetPixelFormat(dc_, pf, &pfd)) throw GraphicsException("opengl: no suitable pixel format");
        rc_ = wcc(dc_);
        if (!rc_) throw GraphicsException("opengl: wglCreateContext failed");
    }
    void close() {
        if (wglMakeCurrent_) wglMakeCurrent_(nullptr, nullptr);
        if (rc_) { auto del = win_fn<BOOL(WINAPI*)(HGLRC)>("wglDeleteContext"); if (del) del(rc_); }
        if (dc_ && hwnd_) ReleaseDC(hwnd_, dc_);
        if (hwnd_) DestroyWindow(hwnd_);
        if (lib_) FreeLibrary(lib_);
    }
    void* proc(const char* n) const {
        void* p = reinterpret_cast<void*>(wglGetProcAddress_(n));
        if (!p || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) || p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) p = reinterpret_cast<void*>(GetProcAddress(lib_, n));
        return p;
    }
#else
    void* egl_ = nullptr;
    void* dpy_ = nullptr;
    void* ctx_ = nullptr;
    unsigned (*eglMakeCurrent__)(void*, void*, void*, void*) = nullptr;
    unsigned eglMakeCurrent_(void* d, void* s, void* r, void* c) const { return eglMakeCurrent__(d, s, r, c); }
    void* (*eglGetProcAddress_)(const char*) = nullptr;
    unsigned (*eglDestroyContext_)(void*, void*) = nullptr;
    unsigned (*eglTerminate_)(void*) = nullptr;

    void open() {
        egl_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!egl_) throw GraphicsException("opengl: libEGL.so.1 not found");
        auto sym = [&](const char* n) { void* p = dlsym(egl_, n); if (!p) throw GraphicsException(std::string("opengl: missing EGL function ") + n); return p; };
        auto getPlatformDisplay = reinterpret_cast<void* (*)(unsigned, void*, const int*)>(dlsym(egl_, "eglGetPlatformDisplay"));
        auto getDisplay = reinterpret_cast<void* (*)(void*)>(sym("eglGetDisplay"));
        auto initialize = reinterpret_cast<unsigned (*)(void*, int*, int*)>(sym("eglInitialize"));
        auto bindAPI = reinterpret_cast<unsigned (*)(unsigned)>(sym("eglBindAPI"));
        auto createContext = reinterpret_cast<void* (*)(void*, void*, void*, const int*)>(sym("eglCreateContext"));
        auto chooseConfig = reinterpret_cast<unsigned (*)(void*, const int*, void**, int, int*)>(sym("eglChooseConfig"));
        eglMakeCurrent__ = reinterpret_cast<unsigned (*)(void*, void*, void*, void*)>(sym("eglMakeCurrent"));
        eglGetProcAddress_ = reinterpret_cast<void* (*)(const char*)>(sym("eglGetProcAddress"));
        eglDestroyContext_ = reinterpret_cast<unsigned (*)(void*, void*)>(sym("eglDestroyContext"));
        eglTerminate_ = reinterpret_cast<unsigned (*)(void*)>(sym("eglTerminate"));
        constexpr unsigned kPlatformSurfacelessMesa = 0x31DD;
        if (getPlatformDisplay) dpy_ = getPlatformDisplay(kPlatformSurfacelessMesa, nullptr, nullptr);
        if (!dpy_) dpy_ = getDisplay(nullptr);
        int maj = 0, min = 0;
        if (!dpy_ || !initialize(dpy_, &maj, &min)) throw GraphicsException("opengl: eglInitialize failed (no display)");
        // desktop GL 3.3 core first, GLES 3.0 as fallback; surfaceless contexts need no config surface type
        constexpr int EGL_NONE = 0x3038, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_BIT = 0x8, EGL_OPENGL_ES3_BIT = 0x40;
        const unsigned EGL_OPENGL_API = 0x30A2, EGL_OPENGL_ES_API = 0x30A0;
        for (int attempt = 0; attempt < 2 && !ctx_; ++attempt) {
            const bool es = attempt == 1;
            if (!bindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API)) continue;
            const int cfg_attr[] = {0x3033 /*EGL_SURFACE_TYPE*/, 0, EGL_RENDERABLE_TYPE, es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT, EGL_NONE};
            void* cfg = nullptr;
            int n = 0;
            if (!chooseConfig(dpy_, cfg_attr, &cfg, 1, &n) || n < 1) continue;
            const int ctx_attr_gl[] = {0x3098 /*MAJOR*/, 3, 0x30FB /*MINOR*/, 3, 0x30FD /*PROFILE_MASK*/, 1 /*CORE*/, EGL_NONE};
            const int ctx_attr_es[] = {0x3098, 3, 0x30FB, 0, EGL_NONE};
            ctx_ = createContext(dpy_, cfg, nullptr, es ? ctx_attr_es : ctx_attr_gl);
        }
        if (!ctx_) throw GraphicsException("opengl: could not create an OpenGL 3.3 core / GLES 3.0 context");
    }
    void close() {
        if (dpy_) {
            if (eglMakeCurrent__) eglMakeCurrent__(dpy_, nullptr, nullptr, nullptr);
            if (ctx_ && eglDestroyContext_) eglDestroyContext_(dpy_, ctx_);
            if (eglTerminate_) eglTerminate_(dpy_);
        }
        if (egl_) dlclose(egl_);
    }
    void* proc(const char* n) const { return eglGetProcAddress_(n); }
#endif
};

inline unsigned wrap_enum(Wrap w) {
    switch (w) {
        case Wrap::clamp: return CLAMP_TO_EDGE;
        case Wrap::mirror: return MIRRORED_REPEAT;
        default: return REPEAT;
    }
}
inline unsigned depth_enum(DepthFunc f) {
    switch (f) {
        case DepthFunc::less: return LESS;
        case DepthFunc::lequal: return LEQUAL;
        case DepthFunc::greater: return GREATER;
        case DepthFunc::gequal: return GEQUAL;
        case DepthFunc::equal: return EQUAL;
        case DepthFunc::not_equal: return NOTEQUAL;
        case DepthFunc::always: return ALWAYS;
        default: return NEVER;
    }
}
inline unsigned internal_format(PixelFormat f) { return f == PixelFormat::rgba8_srgb ? SRGB8_ALPHA8 : f == PixelFormat::r8_unorm ? R8 : RGBA8; }

class GlBuffer final : public Buffer {
public:
    GlBuffer(std::shared_ptr<Context> c, usize size, BufferUsage u, std::span<const std::byte> init) : Buffer(size, u), c_(std::move(c)) {
        if (init.size() > size) throw GraphicsException("initial data larger than the buffer");
        c_->make_current();
        auto& g = c_->gl;
        g.GenBuffers(1, &id_);
        g.BindBuffer(ARRAY_BUFFER, id_);
        std::vector<std::byte> zero;
        if (init.size() == size) g.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(size), init.data(), DYNAMIC_DRAW);
        else {
            zero.assign(size, std::byte{0});
            if (!init.empty()) std::memcpy(zero.data(), init.data(), init.size());
            g.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(size), zero.data(), DYNAMIC_DRAW);
        }
    }
    ~GlBuffer() override { try { c_->make_current(); c_->gl.DeleteBuffers(1, &id_); } catch (...) {} }
    void write(usize offset, std::span<const std::byte> d) override {
        check(offset, d.size());
        c_->make_current();
        c_->gl.BindBuffer(ARRAY_BUFFER, id_);
        c_->gl.BufferSubData(ARRAY_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(d.size()), d.data());
    }
    [[nodiscard]] std::vector<std::byte> read(usize offset, usize n) const override {
        check(offset, n);
        c_->make_current();
        std::vector<std::byte> out(n);
        if (n == 0) return out;
        if (!c_->gl.GetBufferSubData) throw GraphicsException("opengl: buffer read-back is not supported by this driver");
        c_->gl.BindBuffer(ARRAY_BUFFER, id_);
        c_->gl.GetBufferSubData(ARRAY_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(n), out.data());
        return out;
    }
    [[nodiscard]] GLuint id() const noexcept { return id_; }
private:
    std::shared_ptr<Context> c_;
    GLuint id_ = 0;
};

class GlTexture final : public GpuTexture {
public:
    GlTexture(std::shared_ptr<Context> c, const TextureDesc& d, std::span<const std::byte> init) : GpuTexture(d), c_(std::move(c)) {
        if (d.width == 0 || d.height == 0 || d.width > 16384 || d.height > 16384) throw GraphicsException("invalid texture size");
        c_->make_current();
        auto& g = c_->gl;
        g.GenTextures(1, &id_);
        g.BindTexture(TEXTURE_2D, id_);
        g.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        g.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        g.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        g.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        g.PixelStorei(UNPACK_ALIGNMENT, 1);
        const std::vector<std::byte> zero(d.width * d.height * bytes_per_pixel(d.format), std::byte{0});
        if (!init.empty() && init.size() != zero.size()) throw GraphicsException("texture upload: wrong data size");
        upload(init.empty() ? zero.data() : init.data());
    }
    ~GlTexture() override { try { c_->make_current(); c_->gl.DeleteTextures(1, &id_); } catch (...) {} }
    void write(std::span<const std::byte> pixels) override {
        if (pixels.size() != w_ * h_ * bytes_per_pixel(fmt_)) throw GraphicsException("texture upload: wrong data size");
        c_->make_current();
        c_->gl.BindTexture(TEXTURE_2D, id_);
        c_->gl.PixelStorei(UNPACK_ALIGNMENT, 1);
        upload(pixels.data());
    }
    using GpuTexture::write;
    [[nodiscard]] Image read() const override {
        c_->make_current();
        auto& g = c_->gl;
        GLuint fbo = 0;
        g.GenFramebuffers(1, &fbo);
        g.BindFramebuffer(FRAMEBUFFER, fbo);
        g.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, id_, 0);
        if (g.CheckFramebufferStatus(FRAMEBUFFER) != FRAMEBUFFER_COMPLETE) { g.BindFramebuffer(FRAMEBUFFER, 0); g.DeleteFramebuffers(1, &fbo); throw GraphicsException("opengl: texture read-back framebuffer is incomplete"); }
        if (c_->desktop) g.Disable(FRAMEBUFFER_SRGB);
        g.PixelStorei(PACK_ALIGNMENT, 1);
        const bool r8 = fmt_ == PixelFormat::r8_unorm;
        std::vector<u8> raw(w_ * h_ * (r8 ? 1 : 4));
        g.ReadPixels(0, 0, static_cast<GLsizei>(w_), static_cast<GLsizei>(h_), r8 ? RED : RGBA, UNSIGNED_BYTE, raw.data());
        g.BindFramebuffer(FRAMEBUFFER, 0);
        g.DeleteFramebuffers(1, &fbo);
        Image img(w_, h_);
        for (usize y = 0; y < h_; ++y) {
            const usize src_y = c_->clip_control ? y : h_ - 1 - y;  // without clip control the stored rows are bottom-first
            for (usize x = 0; x < w_; ++x) {
                const usize i = src_y * w_ + x;
                img(x, y) = r8 ? Color{raw[i], 0, 0, 255} : Color{raw[4 * i], raw[4 * i + 1], raw[4 * i + 2], raw[4 * i + 3]};
            }
        }
        return img;
    }
    [[nodiscard]] GLuint id() const noexcept { return id_; }
private:
    void upload(const void* data) {
        c_->gl.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(internal_format(fmt_)), static_cast<GLsizei>(w_), static_cast<GLsizei>(h_), 0, fmt_ == PixelFormat::r8_unorm ? RED : RGBA, UNSIGNED_BYTE, data);
    }
    std::shared_ptr<Context> c_;
    GLuint id_ = 0;
};

class GlPipeline final : public Pipeline {
public:
    GlPipeline(std::shared_ptr<Context> c, PipelineDesc d) : Pipeline(std::move(d)), c_(std::move(c)) {
        if (desc_.shader.glsl_vertex.empty() || desc_.shader.glsl_fragment.empty()) throw GraphicsException("opengl backend needs GLSL vertex and fragment sources");
        if (desc_.layout.stride == 0) throw GraphicsException("pipeline vertex layout has a zero stride");
        c_->make_current();
        auto& g = c_->gl;
        const GLuint vs = compile(VERTEX_SHADER, desc_.shader.glsl_vertex, "vertex");
        GLuint fs = 0;
        try { fs = compile(FRAGMENT_SHADER, desc_.shader.glsl_fragment, "fragment"); } catch (...) { g.DeleteShader(vs); throw; }
        prog_ = g.CreateProgram();
        g.AttachShader(prog_, vs);
        g.AttachShader(prog_, fs);
        g.LinkProgram(prog_);
        g.DeleteShader(vs);
        g.DeleteShader(fs);
        GLint ok = 0;
        g.GetProgramiv(prog_, LINK_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            g.GetProgramiv(prog_, INFO_LOG_LENGTH, &len);
            std::string log(static_cast<usize>(std::max(len, 1)), '\0');
            g.GetProgramInfoLog(prog_, len, nullptr, log.data());
            g.DeleteProgram(prog_);
            throw GraphicsException("opengl: program link failed: " + log.substr(0, log.find('\0')));
        }
        g.UseProgram(prog_);
        for (usize i = 0; i < desc_.shader.uniform_blocks.size(); ++i) {
            const GLuint idx = g.GetUniformBlockIndex(prog_, desc_.shader.uniform_blocks[i].c_str());
            if (idx != INVALID_INDEX) g.UniformBlockBinding(prog_, idx, static_cast<GLuint>(i));
        }
        for (usize i = 0; i < desc_.shader.samplers.size(); ++i) {
            const GLint loc = g.GetUniformLocation(prog_, desc_.shader.samplers[i].c_str());
            if (loc >= 0) g.Uniform1i(loc, static_cast<GLint>(i));
        }
    }
    ~GlPipeline() override { try { c_->make_current(); c_->gl.DeleteProgram(prog_); } catch (...) {} }
    [[nodiscard]] GLuint program() const noexcept { return prog_; }
private:
    GLuint compile(GLenum type, const std::string& src, const char* what) {
        auto& g = c_->gl;
        const GLuint s = g.CreateShader(type);
        const char* p = src.c_str();
        g.ShaderSource(s, 1, &p, nullptr);
        g.CompileShader(s);
        GLint ok = 0;
        g.GetShaderiv(s, COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            g.GetShaderiv(s, INFO_LOG_LENGTH, &len);
            std::string log(static_cast<usize>(std::max(len, 1)), '\0');
            g.GetShaderInfoLog(s, len, nullptr, log.data());
            g.DeleteShader(s);
            throw GraphicsException(std::string("opengl: ") + what + " shader compile failed: " + log.substr(0, log.find('\0')));
        }
        return s;
    }
    std::shared_ptr<Context> c_;
    GLuint prog_ = 0;
};

class GlFramebuffer final : public Framebuffer {
public:
    GlFramebuffer(std::shared_ptr<Context> c, std::shared_ptr<GpuTexture> color, bool depth) : Framebuffer(std::move(color), depth), c_(std::move(c)) {
        auto* tex = dynamic_cast<GlTexture*>(color_.get());
        if (!tex) throw GraphicsException("framebuffer texture belongs to a different backend");
        c_->make_current();
        auto& g = c_->gl;
        g.GenFramebuffers(1, &fbo_);
        g.BindFramebuffer(FRAMEBUFFER, fbo_);
        g.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex->id(), 0);
        if (depth) {
            g.GenRenderbuffers(1, &rb_);
            g.BindRenderbuffer(RENDERBUFFER, rb_);
            g.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, static_cast<GLsizei>(color_->width()), static_cast<GLsizei>(color_->height()));
            g.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, rb_);
        }
        const GLenum st = g.CheckFramebufferStatus(FRAMEBUFFER);
        g.BindFramebuffer(FRAMEBUFFER, 0);
        if (st != FRAMEBUFFER_COMPLETE) { release(); throw GraphicsException("opengl: framebuffer is incomplete"); }
    }
    ~GlFramebuffer() override { try { c_->make_current(); release(); } catch (...) {} }
    [[nodiscard]] GLuint fbo() const noexcept { return fbo_; }
private:
    void release() {
        if (rb_) c_->gl.DeleteRenderbuffers(1, &rb_);
        if (fbo_) c_->gl.DeleteFramebuffers(1, &fbo_);
        rb_ = fbo_ = 0;
    }
    std::shared_ptr<Context> c_;
    GLuint fbo_ = 0, rb_ = 0;
};

}  // namespace gl_detail

/// OpenGL implementation of the GPU abstraction.
class OpenGLDevice final : public Device {
public:
    OpenGLDevice() : c_(std::make_shared<gl_detail::Context>()) {
        auto& g = c_->gl;
        g.GenVertexArrays(1, &vao_);
        g.BindVertexArray(vao_);
    }
    ~OpenGLDevice() override { try { c_->make_current(); c_->gl.DeleteVertexArrays(1, &vao_); } catch (...) {} }

    [[nodiscard]] DeviceInfo info() const override {
        c_->make_current();
        auto s = [&](unsigned n) { const auto* p = c_->gl.GetString(n); return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); };
        return {Backend::opengl, s(gl_detail::RENDERER), s(gl_detail::VENDOR), s(gl_detail::VERSION)};
    }
    /// True when the driver reports an upper-left window origin (so render-target textures keep row 0 at the top).
    [[nodiscard]] bool top_left_origin() const noexcept { return c_->clip_control; }

    std::shared_ptr<Buffer> create_buffer(const BufferDesc& d, std::span<const std::byte> initial = {}) override {
        if (d.size == 0) throw GraphicsException("buffer size must be non-zero");
        return std::make_shared<gl_detail::GlBuffer>(c_, d.size, d.usage, initial);
    }
    std::shared_ptr<GpuTexture> create_texture(const TextureDesc& d, std::span<const std::byte> initial = {}) override {
        return std::make_shared<gl_detail::GlTexture>(c_, d, initial);
    }
    std::shared_ptr<Pipeline> create_pipeline(const PipelineDesc& d) override {
        for (const auto& a : d.layout.attributes)
            if (a.offset + vertex_format_size(a.format) > d.layout.stride) throw GraphicsException("vertex attribute exceeds the vertex stride");
        return std::make_shared<gl_detail::GlPipeline>(c_, d);
    }
    std::shared_ptr<Framebuffer> create_framebuffer(std::shared_ptr<GpuTexture> color, bool with_depth) override {
        if (!color || !color->is_render_target()) throw GraphicsException("framebuffer colour attachment must be a render-target texture");
        if (color->format() == PixelFormat::r8_unorm) throw GraphicsException("r8 textures cannot be rendered to");
        return std::make_shared<gl_detail::GlFramebuffer>(c_, std::move(color), with_depth);
    }
    void finish() override { c_->make_current(); c_->gl.Finish(); }

    void submit(const CommandList& list) override {
        using namespace gl_detail;
        if (!list.complete()) throw GraphicsException("command list has an unfinished render pass");
        c_->make_current();
        auto& g = c_->gl;
        g.BindVertexArray(vao_);
        const GlFramebuffer* fb = nullptr;
        const GlPipeline* pipe = nullptr;
        std::shared_ptr<Buffer> vb, ib;
        usize vb_off = 0, ib_off = 0;
        IndexType itype = IndexType::u16;
        bool enabled_attr[16] = {};
        std::vector<GLuint> samplers;  // sampler objects are emulated with per-texture parameters (GLES3/GL3.3 both support it)
        for (const Command& c : list.commands()) {
            if (const auto* b = std::get_if<cmd::BeginPass>(&c)) {
                fb = dynamic_cast<const GlFramebuffer*>(b->pass.target.get());
                if (!fb) throw GraphicsException("framebuffer belongs to a different backend");
                g.BindFramebuffer(FRAMEBUFFER, fb->fbo());
                g.Viewport(0, 0, static_cast<GLsizei>(fb->width()), static_cast<GLsizei>(fb->height()));
                g.Disable(SCISSOR_TEST);
                if (c_->desktop) g.Disable(FRAMEBUFFER_SRGB);  // clear values are raw bytes on every backend
                g.ColorMask(1, 1, 1, 1);
                g.DepthMask(1);
                GLbitfield bits = 0;
                if (b->pass.clear_color) {
                    const Color cc = *b->pass.clear_color;
                    g.ClearColor(cc.r / 255.0f, cc.g / 255.0f, cc.b / 255.0f, cc.a / 255.0f);
                    bits |= COLOR_BUFFER_BIT;
                }
                if (b->pass.clear_depth && fb->has_depth()) { g.ClearDepth(*b->pass.clear_depth); bits |= DEPTH_BUFFER_BIT; }
                if (bits) g.Clear(bits);
                pipe = nullptr;
            } else if (std::holds_alternative<cmd::EndPass>(c)) {
                g.BindFramebuffer(FRAMEBUFFER, 0);
                fb = nullptr;
            } else if (const auto* p = std::get_if<cmd::SetPipeline>(&c)) {
                pipe = dynamic_cast<const GlPipeline*>(p->pipeline.get());
                if (!pipe) throw GraphicsException("pipeline belongs to a different backend");
                apply_state(*pipe, *fb);
            } else if (const auto* v = std::get_if<cmd::SetVertexBuffer>(&c)) {
                vb = v->buffer, vb_off = v->offset;
            } else if (const auto* i = std::get_if<cmd::SetIndexBuffer>(&c)) {
                ib = i->buffer, itype = i->type, ib_off = i->offset;
            } else if (const auto* u = std::get_if<cmd::SetUniform>(&c)) {
                auto* gb = dynamic_cast<GlBuffer*>(u->buffer.get());
                if (!gb) throw GraphicsException("uniform buffer belongs to a different backend");
                const usize n = u->size ? u->size : gb->size() - std::min(u->offset, gb->size());
                if (u->offset + n > gb->size()) throw GraphicsException("uniform range outside the buffer");
                if (u->offset % static_cast<usize>(c_->uniform_alignment)) throw GraphicsException("uniform offset must be a multiple of " + std::to_string(c_->uniform_alignment) + " bytes on the OpenGL backend");
                g.BindBufferRange(UNIFORM_BUFFER, u->slot, gb->id(), static_cast<GLintptr>(u->offset), static_cast<GLsizeiptr>(n));
            } else if (const auto* t = std::get_if<cmd::SetTexture>(&c)) {
                auto* gt = dynamic_cast<GlTexture*>(t->texture.get());
                if (!gt) throw GraphicsException("texture belongs to a different backend");
                g.ActiveTexture(TEXTURE0 + t->slot);
                g.BindTexture(TEXTURE_2D, gt->id());
                if (t->sampler.mipmaps) g.GenerateMipmap(TEXTURE_2D);
                const bool mip = t->sampler.mipmaps;
                unsigned minf = t->sampler.min == FilterMode::nearest ? (mip ? NEAREST_MIPMAP_LINEAR : NEAREST) : (mip ? LINEAR_MIPMAP_LINEAR : LINEAR);
                g.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, static_cast<GLint>(minf));
                g.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, static_cast<GLint>(t->sampler.mag == FilterMode::nearest ? NEAREST : LINEAR));
                g.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, static_cast<GLint>(wrap_enum(t->sampler.wrap_s)));
                g.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, static_cast<GLint>(wrap_enum(t->sampler.wrap_t)));
            } else if (const auto* sv = std::get_if<cmd::SetViewport>(&c)) {
                const float y = c_->clip_control ? sv->y : static_cast<float>(fb->height()) - sv->y - sv->h;
                g.Viewport(static_cast<GLint>(sv->x), static_cast<GLint>(y), static_cast<GLsizei>(sv->w), static_cast<GLsizei>(sv->h));
                g.Enable(SCISSOR_TEST);
                g.Scissor(static_cast<GLint>(sv->x), static_cast<GLint>(y), static_cast<GLsizei>(sv->w), static_cast<GLsizei>(sv->h));
            } else if (const auto* dr = std::get_if<cmd::Draw>(&c)) {
                setup_attributes(pipe, vb.get(), vb_off, enabled_attr);
                g.DrawArrays(topology(pipe->desc().topology), static_cast<GLint>(dr->first_vertex), static_cast<GLsizei>(dr->vertex_count));
            } else if (const auto* di = std::get_if<cmd::DrawIndexed>(&c)) {
                if (!ib) throw GraphicsException("draw_indexed without an index buffer");
                setup_attributes(pipe, vb.get(), vb_off, enabled_attr, di->base_vertex, itype);
                auto* gi = static_cast<GlBuffer*>(ib.get());
                g.BindBuffer(ELEMENT_ARRAY_BUFFER, gi->id());
                const usize isz = itype == IndexType::u16 ? 2 : 4;
                const void* off = reinterpret_cast<const void*>(ib_off + usize(di->first_index) * isz);
                const GLenum it = itype == IndexType::u16 ? UNSIGNED_SHORT : UNSIGNED_INT;
                g.DrawElements(topology(pipe->desc().topology), static_cast<GLsizei>(di->index_count), it, off);
            }
        }
        const GLenum err = g.GetError();
        if (err != NO_ERROR_) throw GraphicsException("opengl: error 0x" + to_hex(err) + " while executing the command list");
    }

private:
    static std::string to_hex(unsigned v) { char b[16]; std::snprintf(b, sizeof b, "%X", v); return b; }
    static unsigned topology(Topology t) { return t == Topology::triangles ? gl_detail::TRIANGLES : t == Topology::triangle_strip ? gl_detail::TRIANGLE_STRIP : gl_detail::TRIANGLE_FAN; }

    void apply_state(const gl_detail::GlPipeline& p, const gl_detail::GlFramebuffer& fb) {
        using namespace gl_detail;
        auto& g = c_->gl;
        const PipelineDesc& d = p.desc();
        g.UseProgram(p.program());
        if (d.cull == CullMode::none) g.Disable(CULL_FACE);
        else { g.Enable(CULL_FACE); g.CullFace(d.cull == CullMode::back ? BACK : FRONT); }
        g.FrontFace(d.front_ccw ? CCW : CW);
        if (d.depth_test && fb.has_depth()) { g.Enable(DEPTH_TEST); g.DepthFunc(depth_enum(d.depth_func)); } else g.Disable(DEPTH_TEST);
        g.DepthMask(d.depth_write && fb.has_depth() ? 1 : 0);
        if (d.blend) { g.Enable(BLEND); g.BlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA); } else g.Disable(BLEND);
        if (c_->desktop) {
            if (fb.color()->format() == PixelFormat::rgba8_srgb) g.Enable(FRAMEBUFFER_SRGB); else g.Disable(FRAMEBUFFER_SRGB);
            if (g.PolygonMode) g.PolygonMode(FRONT_AND_BACK, d.polygon == PolygonMode::fill ? FILL : d.polygon == PolygonMode::line ? LINE : POINT);
        }
    }

    void setup_attributes(const gl_detail::GlPipeline* p, Buffer* vbuf, usize vb_off, bool* enabled, i32 base_vertex = 0, IndexType = IndexType::u16) {
        using namespace gl_detail;
        if (!p) throw GraphicsException("draw without a pipeline");
        if (!vbuf) throw GraphicsException("draw without a vertex buffer");
        auto& g = c_->gl;
        const VertexLayout& l = p->desc().layout;
        g.BindBuffer(ARRAY_BUFFER, static_cast<GlBuffer*>(vbuf)->id());
        bool used[16] = {};
        // base_vertex is applied by offsetting the attribute pointers (works without glDrawElementsBaseVertex)
        const std::ptrdiff_t base = static_cast<std::ptrdiff_t>(base_vertex) * static_cast<std::ptrdiff_t>(l.stride);
        for (const VertexAttribute& a : l.attributes) {
            if (a.location >= 16) throw GraphicsException("vertex attribute location out of range");
            used[a.location] = true;
            if (!enabled[a.location]) { g.EnableVertexAttribArray(a.location); enabled[a.location] = true; }
            const bool bytes = a.format == VertexFormat::unorm8x4;
            const GLint comps = bytes ? 4 : static_cast<GLint>(vertex_format_size(a.format) / 4);
            const std::ptrdiff_t off = static_cast<std::ptrdiff_t>(vb_off + a.offset) + base;
            if (off < 0) throw GraphicsException("negative vertex offset");
            g.VertexAttribPointer(a.location, comps, bytes ? UNSIGNED_BYTE : FLOAT, bytes ? 1 : 0, static_cast<GLsizei>(l.stride), reinterpret_cast<const void*>(off));
        }
        for (u32 i = 0; i < 16; ++i)
            if (enabled[i] && !used[i]) { g.DisableVertexAttribArray(i); enabled[i] = false; }
    }

    std::shared_ptr<gl_detail::Context> c_;
    gl_detail::GLuint vao_ = 0;
};

/// Create an OpenGL device; throws GraphicsException when no usable driver/context is available.
[[nodiscard]] inline std::unique_ptr<Device> create_opengl_device() { return std::make_unique<OpenGLDevice>(); }

}  // namespace flyg::graphics::gpu

#endif  // FLYG_GPU_OPENGL
