# Cross-compile flyG tests/examples from Linux to Windows PE (x86_64) with mingw-w64.
#   cmake -S . -B build-pe -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-x86_64.cmake
# The resulting .exe files run under wine (used as CMAKE_CROSSCOMPILING_EMULATOR by ctest).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The posix-thread flavour is required for std::thread / std::mutex.
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

find_program(FLYG_WINE wine)
if(FLYG_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${FLYG_WINE})
endif()
