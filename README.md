# flyG

Header-only C++23 library (GPL-2.0-only): `core`, `system`, `memory`, `graphics`, `network`, `data`, `math`.
Targets Windows (PE) and Linux (ELF); exceptions derive from `flyg::Exception`. Self-implemented; no required dependencies.

    add_subdirectory(flyG)
    target_link_libraries(app PRIVATE flyg::flyg)

CMake options: `FLYG_BUILD_TESTS`, `FLYG_BUILD_EXAMPLES`, `FLYG_SANITIZE`, `FLYG_WARNINGS_AS_ERRORS`, `FLYG_GPU_OPENGL` (optional OpenGL backend, default OFF).
Docs: `docs/NET.md`, `docs/GRAPHICS.md`. Examples in `examples/`.
