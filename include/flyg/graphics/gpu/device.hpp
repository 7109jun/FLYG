// flyG - graphics module: GPU abstraction umbrella + backend factory
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <flyg/graphics/gpu/gpu.hpp>
#include <flyg/graphics/gpu/opengl.hpp>
#include <flyg/graphics/gpu/software.hpp>

namespace flyg::graphics::gpu {

/// True when the backend was compiled in (the OpenGL backend additionally needs a working driver at run time).
[[nodiscard]] constexpr bool backend_compiled(Backend b) noexcept {
    if (b == Backend::software) return true;
#if defined(FLYG_GPU_OPENGL)
    return b == Backend::opengl;
#else
    return false;
#endif
}

[[nodiscard]] inline std::unique_ptr<Device> create_device(Backend b = Backend::software) {
    switch (b) {
        case Backend::software: return create_software_device();
        case Backend::opengl:
#if defined(FLYG_GPU_OPENGL)
            return create_opengl_device();
#else
            throw GraphicsException("the OpenGL backend was not compiled in (define FLYG_GPU_OPENGL / CMake -DFLYG_GPU_OPENGL=ON)");
#endif
    }
    throw GraphicsException("unknown GPU backend");
}

}  // namespace flyg::graphics::gpu
