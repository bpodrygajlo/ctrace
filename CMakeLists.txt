cmake_minimum_required(VERSION 3.20)
project(ctrace LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Backend selection
set(CTRACE_BACKEND "stdout" CACHE STRING "Trace backend: stdout | bin | bin-zstd | sqlite | ctf")
set_property(CACHE CTRACE_BACKEND PROPERTY STRINGS "stdout" "bin" "bin-zstd" "sqlite" "ctf")

# Options for external library dependencies
option(CTRACE_ENABLE_ZSTD "Enable Zstandard compressed trace backend (bin-zstd)" ON)
option(CTRACE_ENABLE_SQLITE "Enable SQLite3 database backend" ON)
option(CTRACE_FORCE_BUILTIN_JSON "Use built-in JSON parser in tools instead of system nlohmann_json" OFF)

# Dependencies
find_package(Threads REQUIRED)

set(CTRACE_HAVE_ZSTD FALSE)
if(CTRACE_ENABLE_ZSTD)
    find_library(ZSTD_LIB NAMES zstd)
    if(ZSTD_LIB)
        set(CTRACE_HAVE_ZSTD TRUE)
        message(STATUS "ctrace: Zstandard library found (${ZSTD_LIB}) -> 'bin-zstd' backend enabled")
    else()
        message(STATUS "ctrace: Zstandard library not found -> 'bin-zstd' backend disabled")
    endif()
else()
    message(STATUS "ctrace: Zstandard support disabled via CTRACE_ENABLE_ZSTD=OFF")
endif()

set(CTRACE_HAVE_SQLITE FALSE)
if(CTRACE_ENABLE_SQLITE)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(CTRACE_HAVE_SQLITE TRUE)
        message(STATUS "ctrace: SQLite3 found (${SQLite3_VERSION}) -> 'sqlite' backend enabled")
    else()
        message(STATUS "ctrace: SQLite3 not found -> 'sqlite' backend disabled")
    endif()
else()
    message(STATUS "ctrace: SQLite3 support disabled via CTRACE_ENABLE_SQLITE=OFF")
endif()

# Validate default backend selection
if(CTRACE_BACKEND STREQUAL "bin-zstd" AND NOT CTRACE_HAVE_ZSTD)
    message(FATAL_ERROR "ctrace error: Default backend is set to 'bin-zstd', but Zstandard is not available. Please install libzstd-dev or set -DCTRACE_BACKEND=stdout / bin / ctf.")
endif()
if(CTRACE_BACKEND STREQUAL "sqlite" AND NOT CTRACE_HAVE_SQLITE)
    message(FATAL_ERROR "ctrace error: Default backend is set to 'sqlite', but SQLite3 is not available. Please install libsqlite3-dev or set -DCTRACE_BACKEND=stdout / bin / ctf.")
endif()

# Find Clang / LLVM for ctrace-scan
find_package(Clang CONFIG PATHS /usr/lib/llvm-18/lib/cmake/clang /usr/lib/llvm-17/lib/cmake/clang NO_DEFAULT_PATH)
if(NOT Clang_FOUND)
    find_package(Clang REQUIRED CONFIG)
endif()

find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND AND NOT CTRACE_FORCE_BUILTIN_JSON)
    message(STATUS "ctrace: Using system nlohmann_json for build tools")
else()
    message(STATUS "ctrace: Using built-in C++17 JSON implementation (tools/common/ctrace_json.hpp)")
endif()

# Header-only interface target
add_library(ctrace_headers INTERFACE)
target_include_directories(ctrace_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
add_library(ctrace::headers ALIAS ctrace_headers)
add_library(ctrace::ctrace_headers ALIAS ctrace_headers)

# Tool 1: ctrace-scan (AST Extractor)
add_executable(ctrace-scan tools/ctrace-scan/main.cpp)
target_include_directories(ctrace-scan PRIVATE ${CLANG_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/tools/common)
target_link_libraries(ctrace-scan PRIVATE clang-cpp LLVM)
if(CTRACE_FORCE_BUILTIN_JSON)
    target_compile_definitions(ctrace-scan PRIVATE CTRACE_FORCE_BUILTIN_JSON)
elseif(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(ctrace-scan PRIVATE nlohmann_json::nlohmann_json)
endif()
add_executable(ctrace::ctrace-scan ALIAS ctrace-scan)

# Tool 2: ctrace-gen (Schema Merge & Code Generator)
add_executable(ctrace-gen tools/ctrace-gen/main.cpp)
target_include_directories(ctrace-gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/common)
if(CTRACE_FORCE_BUILTIN_JSON)
    target_compile_definitions(ctrace-gen PRIVATE CTRACE_FORCE_BUILTIN_JSON)
elseif(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(ctrace-gen PRIVATE nlohmann_json::nlohmann_json)
endif()
add_executable(ctrace::ctrace-gen ALIAS ctrace-gen)

# Runtime libraries
# 1. ctrace_rt_stdout (Pure C / POSIX, no external dependencies)
add_library(ctrace_rt_stdout STATIC runtime/rt_common.c)
target_compile_definitions(ctrace_rt_stdout PRIVATE CTRACE_RUNTIME)
target_compile_options(ctrace_rt_stdout PRIVATE -Wall -Wextra)
target_link_libraries(ctrace_rt_stdout PUBLIC ctrace_headers Threads::Threads)
add_library(ctrace::ctrace_rt_stdout ALIAS ctrace_rt_stdout)

# 2. ctrace_rt_bin (Pure C / POSIX, no external dependencies)
add_library(ctrace_rt_bin STATIC runtime/rt_common.c runtime/rt_ringbuf.c)
target_compile_definitions(ctrace_rt_bin PRIVATE CTRACE_RUNTIME CTRACE_BACKEND_CUSTOM)
target_compile_options(ctrace_rt_bin PRIVATE -Wall -Wextra)
target_link_libraries(ctrace_rt_bin PUBLIC ctrace_headers Threads::Threads)
add_library(ctrace::ctrace_rt_bin ALIAS ctrace_rt_bin)

# 3. ctrace_rt_ctf (Common Trace Format, no external dependencies)
add_library(ctrace_rt_ctf STATIC runtime/rt_common.c runtime/rt_ctf.c)
target_compile_definitions(ctrace_rt_ctf PRIVATE CTRACE_RUNTIME CTRACE_BACKEND_CUSTOM)
target_compile_options(ctrace_rt_ctf PRIVATE -Wall -Wextra)
target_link_libraries(ctrace_rt_ctf PUBLIC ctrace_headers Threads::Threads)
add_library(ctrace::ctrace_rt_ctf ALIAS ctrace_rt_ctf)

# 4. ctrace_rt_bin-zstd (Optional: requires zstd)
if(CTRACE_HAVE_ZSTD)
    add_library(ctrace_rt_bin-zstd STATIC runtime/rt_common.c runtime/rt_zstd.c)
    target_compile_definitions(ctrace_rt_bin-zstd PRIVATE CTRACE_RUNTIME CTRACE_BACKEND_CUSTOM)
    target_compile_options(ctrace_rt_bin-zstd PRIVATE -Wall -Wextra)
    target_link_libraries(ctrace_rt_bin-zstd PUBLIC ctrace_headers Threads::Threads ${ZSTD_LIB})
    add_library(ctrace_rt_bin_zstd ALIAS ctrace_rt_bin-zstd)
    add_library(ctrace::ctrace_rt_bin-zstd ALIAS ctrace_rt_bin-zstd)
    add_library(ctrace::ctrace_rt_bin_zstd ALIAS ctrace_rt_bin-zstd)
endif()

# 5. ctrace_rt_sqlite (Optional: requires SQLite3)
if(CTRACE_HAVE_SQLITE)
    add_library(ctrace_rt_sqlite STATIC runtime/rt_common.c runtime/rt_sqlite.c)
    target_compile_definitions(ctrace_rt_sqlite PRIVATE CTRACE_RUNTIME CTRACE_BACKEND_CUSTOM CTRACE_BACKEND_SQLITE)
    target_compile_options(ctrace_rt_sqlite PRIVATE -Wall -Wextra)
    target_link_libraries(ctrace_rt_sqlite PUBLIC ctrace_headers Threads::Threads SQLite::SQLite3)
    add_library(ctrace::ctrace_rt_sqlite ALIAS ctrace_rt_sqlite)
endif()

# 6. ctrace_rt_runtime (Multi-backend runtime selectable via CTRACE_BACKEND env var)
set(CTRACE_RT_RUNTIME_SRCS
    runtime/rt_runtime.c
    runtime/rt_common.c
    runtime/rt_ringbuf.c
    runtime/rt_ctf.c
)
add_library(ctrace_rt_runtime STATIC ${CTRACE_RT_RUNTIME_SRCS})
target_compile_definitions(ctrace_rt_runtime PRIVATE CTRACE_RUNTIME CTRACE_BACKEND_CUSTOM CTRACE_MULTI_BACKEND)
target_compile_options(ctrace_rt_runtime PRIVATE -Wall -Wextra)
target_link_libraries(ctrace_rt_runtime PUBLIC ctrace_headers Threads::Threads)
add_library(ctrace::ctrace_rt_runtime ALIAS ctrace_rt_runtime)
add_library(ctrace_rt_multi ALIAS ctrace_rt_runtime)
add_library(ctrace::ctrace_rt_multi ALIAS ctrace_rt_runtime)

# Include the CMake integration function (CACHE INTERNAL so parent projects via CPM / add_subdirectory always see it)
set(CTRACE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE INTERNAL "Path to ctrace public include directory" FORCE)
include(cmake/ctrace.cmake)

# Installation & CMake package export
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS ctrace-scan ctrace-gen
    EXPORT ctraceTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(CTRACE_INSTALL_TARGETS ctrace_headers ctrace_rt_stdout ctrace_rt_bin ctrace_rt_ctf ctrace_rt_runtime)
if(CTRACE_HAVE_ZSTD)
    list(APPEND CTRACE_INSTALL_TARGETS ctrace_rt_bin-zstd)
endif()
if(CTRACE_HAVE_SQLITE)
    list(APPEND CTRACE_INSTALL_TARGETS ctrace_rt_sqlite)
endif()

install(TARGETS ${CTRACE_INSTALL_TARGETS}
    EXPORT ctraceTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/ctrace.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ctrace)

install(EXPORT ctraceTargets
    FILE ctraceTargets.cmake
    NAMESPACE ctrace::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ctrace
)

set(CTRACE_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ctrace")
configure_package_config_file(
    cmake/ctraceConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ctraceConfig.cmake
    INSTALL_DESTINATION ${CTRACE_CONFIG_INSTALL_DIR}
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ctraceConfigVersion.cmake
    VERSION 1.0.0
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ctraceConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ctraceConfigVersion.cmake
    DESTINATION ${CTRACE_CONFIG_INSTALL_DIR}
)

# Export for build tree usage
export(EXPORT ctraceTargets
    NAMESPACE ctrace::
    FILE "${CMAKE_CURRENT_BINARY_DIR}/ctraceTargets.cmake"
)

# Tests
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
    add_subdirectory(tests)
endif()
