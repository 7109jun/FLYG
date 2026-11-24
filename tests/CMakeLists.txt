# ---- unit tests -----------------------------------------------------------
add_executable(flyg_tests
    main.cpp
    test_core.cpp
    test_memory.cpp
    test_math_basic.cpp
    test_math_advanced.cpp
    test_system.cpp
    test_data_json.cpp
    test_data_csv_xml.cpp
    test_data_yaml.cpp
    test_data_toml.cpp
    test_data_binary.cpp
    test_data_kvstore.cpp
    test_data_collection.cpp
    test_data_hash.cpp
    test_net_basic.cpp
    test_net_dns_async.cpp
    test_net_http_ws.cpp
    test_net_tls_crypto.cpp
    test_net_x509.cpp
    test_net_tls.cpp
    test_data_compress.cpp
    test_graphics_codecs.cpp
    test_graphics_canvas.cpp
    test_graphics_3d.cpp
    test_graphics_gpu.cpp)
target_link_libraries(flyg_tests PRIVATE flyg flyg_dev_options Threads::Threads)
set_target_properties(flyg_tests PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME flyg_tests COMMAND flyg_tests)

# ---- header self-containment + ODR check ---------------------------------
# One translation unit per public header, each including *only* that header, all linked
# into a single executable:
#   * a header that is not self-contained fails to compile,
#   * a non-inline definition in a header causes a multiple-definition link error.
file(GLOB_RECURSE FLYG_PUBLIC_HEADERS RELATIVE ${PROJECT_SOURCE_DIR}/include
    CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/flyg/*.hpp)

set(FLYG_HEADER_CHECK_SOURCES "")
set(_flyg_idx 0)
foreach(_hdr IN LISTS FLYG_PUBLIC_HEADERS)
    set(_src ${CMAKE_CURRENT_BINARY_DIR}/header_check/check_${_flyg_idx}.cpp)
    file(GENERATE OUTPUT ${_src}
        CONTENT "// generated: includes only <${_hdr}>\n#include <${_hdr}>\nint flyg_header_check_${_flyg_idx}() { return ${_flyg_idx}; }\n")
    list(APPEND FLYG_HEADER_CHECK_SOURCES ${_src})
    math(EXPR _flyg_idx "${_flyg_idx} + 1")
endforeach()

set(_flyg_main ${CMAKE_CURRENT_BINARY_DIR}/header_check/main.cpp)
set(_flyg_decls "")
set(_flyg_calls "")
math(EXPR _flyg_last "${_flyg_idx} - 1")
foreach(_i RANGE ${_flyg_last})
    string(APPEND _flyg_decls "int flyg_header_check_${_i}();\n")
    string(APPEND _flyg_calls "    sum += flyg_header_check_${_i}();\n")
endforeach()
file(GENERATE OUTPUT ${_flyg_main}
    CONTENT "${_flyg_decls}\nint main() {\n    int sum = 0;\n${_flyg_calls}    return sum >= 0 ? 0 : 1;\n}\n")

add_executable(flyg_header_check ${FLYG_HEADER_CHECK_SOURCES} ${_flyg_main})
target_link_libraries(flyg_header_check PRIVATE flyg flyg_dev_options)
set_target_properties(flyg_header_check PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME flyg_header_check COMMAND flyg_header_check)

if(WIN32 AND CMAKE_CROSSCOMPILING)
    set_tests_properties(flyg_tests flyg_header_check PROPERTIES
        ENVIRONMENT "WINEDEBUG=-all;WINEPREFIX=${CMAKE_BINARY_DIR}/wineprefix")
endif()
