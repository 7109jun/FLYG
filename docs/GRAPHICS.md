# flyG graphics module

Header-only, self-implemented, GPL-2.0. Include `<flyg/graphics/graphics.hpp>` (everything) or single headers.

| Area | Headers | Notes |
|---|---|---|
| Image codecs | `codec/{png,bmp,pnm,qoi,tga,hdr,jpeg,gif,codec}.hpp` | PNG (all colour types/bit depths, Adam7), BMP, PNM, QOI, TGA, Radiance HDR, baseline+progressive JPEG (encode: baseline), GIF incl. animation. `decode_image` / `load_image` detect the format. |
| 2D canvas | `canvas.hpp path.hpp stroke.hpp raster.hpp paint.hpp` | Anti-aliased scanline fill (non-zero/even-odd), strokes (caps, joins, dashes), SVG path parser, linear/radial/conic gradients, 16 blend modes, clip, transforms. |
| Text | `font.hpp` | TrueType (glyf) outlines, kerning (`kern`), built-in 5x7 bitmap font. |
| 3D (CPU) | `mesh.hpp texture.hpp renderer.hpp shaders.hpp` | Perspective-correct software rasteriser, depth buffer, mip-mapped textures, shaders as C++ callables. |
| GPU abstraction | `gpu/gpu.hpp gpu/device.hpp` | Buffers, textures, pipelines, framebuffers, command lists; two backends. |
| Backends | `gpu/software.hpp`, `gpu/opengl.hpp` | Software always available. OpenGL is optional (`-DFLYG_GPU_OPENGL=ON`). |

## GPU conventions (identical on both backends)
Clip space y-up, z in [-1, 1]; window origin top-left; CCW triangles are front-facing; texture row 0 is the top;
fragment colours are written as-is to `rgba8_unorm` targets and sRGB-encoded for `rgba8_srgb` targets;
blending is source-over; device calls must come from the creating thread.

A pipeline carries both a C++ shader (`ShaderSource::software`) and GLSL sources (`glsl_vertex/glsl_fragment`,
`uniform_blocks`, `samplers` for slot naming); each backend uses the part it needs. Uniform offsets on OpenGL must be
multiples of the driver's UBO alignment (usually 256) — the software backend has no such limit.

## OpenGL backend
No GL headers or link-time GL libraries: the driver is loaded at run time (Linux: `libEGL.so.1`, surfaceless context,
desktop GL 3.3 core preferred, GLES 3.0 fallback; Windows: `opengl32.dll` + hidden window, WGL). Needs `${CMAKE_DL_LIBS}`
(added by the CMake option). If no context can be created `create_device(Backend::opengl)` throws `GraphicsException`.
Windows/WGL is compile-checked only (mingw), never executed in this project's CI.

## Known limitations
- Software rasteriser is single-threaded. No OpenType/CFF fonts, GPOS kerning or text shaping.
- JPEG: no arithmetic coding, 12-bit or lossless modes. TIFF/ICO not implemented.
- No Vulkan backend (no SPIR-V toolchain in a header-only, dependency-free library).
- Built-in 5x7 font is an original transcription of a classic public-domain style bitmap font; no third-party font data is shipped.
