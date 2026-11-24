cmake_minimum_required(VERSION 3.20)

project(flyG
    VERSION 0.1.0
    DESCRIPTION "flyG - general purpose C++23 header-only native library"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(FLYG_IS_TOP_LEVEL ON)
else()
    set(FLYG_IS_TOP_LEVEL OFF)
endif()

option(FLYG_BUILD_TESTS "Build the flyG unit tests" ${FLYG_IS_TOP_LEVEL})
option(FLYG_BUILD_EXAMPLES "Build the flyG examples" ${FLYG_IS_TOP_LEVEL})
option(FLYG_SANITIZE "Build tests/examples with AddressSanitizer + UBSan (GCC/Clang on Linux)" OFF)
option(FLYG_WARNINGS_AS_ERRORS "Treat warnings in tests/examples as errors" OFF)
option(FLYG_STATIC_RUNTIME "Link tests/examples statically (default for MinGW so wine needs no DLLs)" ${MINGW})
option(FLYG_GPU_OPENGL "Compile the optional OpenGL GPU backend (driver is loaded at run time via EGL/WGL; no GL headers or libs needed)" OFF)
option(FLYG_INSTALL "Generate install rules" ${FLYG_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# The library: header-only, so an INTERFACE target.
# ---------------------------------------------------------------------------
add_library(flyg INTERFACE)
add_library(flyg::flyg ALIAS flyg)
target_include_directories(flyg INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(flyg INTERFACE cxx_std_23)
find_package(Threads REQUIRED)
target_link_libraries(flyg INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(flyg INTERFACE ws2_32 iphlpapi bcrypt crypt32)
endif()

if(FLYG_GPU_OPENGL)
    target_compile_definitions(flyg INTERFACE FLYG_GPU_OPENGL)
    target_link_libraries(flyg INTERFACE ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(flyg INTERFACE user32 gdi32)
    endif()
endif()

# Windows system libraries are added here as the modules that need them are implemented.

# ---------------------------------------------------------------------------
# Build settings used only by flyG's own tests/examples (never exported).
# ---------------------------------------------------------------------------
add_library(flyg_dev_options INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(flyg_dev_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-psabi)
    if(FLYG_WARNINGS_AS_ERRORS)
        target_compile_options(flyg_dev_options INTERFACE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(flyg_dev_options INTERFACE /W4 /permissive-)
    if(FLYG_WARNINGS_AS_ERRORS)
        target_compile_options(flyg_dev_options INTERFACE /WX)
    endif()
endif()
if(FLYG_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
    target_compile_options(flyg_dev_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(flyg_dev_options INTERFACE -fsanitize=address,undefined)
endif()
if(FLYG_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(flyg_dev_options INTERFACE -static)
endif()

if(FLYG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FLYG_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Install / package config
# ---------------------------------------------------------------------------
if(FLYG_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.hpp")
    install(TARGETS flyg EXPORT flygTargets)
    install(EXPORT flygTargets NAMESPACE flyg:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flyg)

    configure_package_config_file(cmake/flygConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/flygConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flyg)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/flygConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/flygConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/flygConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flyg)
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/flyg)
endif()
