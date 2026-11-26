# ctrace.cmake - Public CMake integration API for ctrace

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Enable compile commands" FORCE)

if(NOT DEFINED CTRACE_BACKEND)
    set(CTRACE_BACKEND "stdout" CACHE STRING "Trace backend: stdout | bin | bin-zstd | sqlite | ctf")
    set_property(CACHE CTRACE_BACKEND PROPERTY STRINGS "stdout" "bin" "bin-zstd" "sqlite" "ctf")
endif()

if(NOT DEFINED CTRACE_INCLUDE_DIR OR "${CTRACE_INCLUDE_DIR}" STREQUAL "")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../include/ctrace/trace.h")
        get_filename_component(CTRACE_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include" ABSOLUTE)
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../include/ctrace/trace.h")
        get_filename_component(CTRACE_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../include" ABSOLUTE)
    endif()
endif()

# Ensure compile_commands.json is a valid JSON array so Clang doesn't error on empty file
if(NOT EXISTS "${CMAKE_BINARY_DIR}/compile_commands.json")
    file(WRITE "${CMAKE_BINARY_DIR}/compile_commands.json" "[]\n")
endif()

function(target_enable_ctrace TARGET_NAME)
    cmake_parse_arguments(ARG "" "BACKEND" "" ${ARGN})
    if(ARG_BACKEND)
        set(ACTIVE_BACKEND "${ARG_BACKEND}")
    else()
        set(ACTIVE_BACKEND "${CTRACE_BACKEND}")
    endif()

    # Determine tool targets (handling both in-tree and imported package targets)
    if(TARGET ctrace::ctrace-scan)
        set(CTRACE_SCAN_TARGET ctrace::ctrace-scan)
    else()
        set(CTRACE_SCAN_TARGET ctrace-scan)
    endif()

    if(TARGET ctrace::ctrace-gen)
        set(CTRACE_GEN_TARGET ctrace::ctrace-gen)
    else()
        set(CTRACE_GEN_TARGET ctrace-gen)
    endif()

    if(TARGET ctrace::ctrace_rt_${ACTIVE_BACKEND})
        set(CTRACE_RT_TARGET ctrace::ctrace_rt_${ACTIVE_BACKEND})
    else()
        set(CTRACE_RT_TARGET ctrace_rt_${ACTIVE_BACKEND})
    endif()

    if(NOT TARGET ${CTRACE_RT_TARGET})
        if(ACTIVE_BACKEND STREQUAL "sqlite")
            message(FATAL_ERROR "ctrace error: Target '${TARGET_NAME}' requested backend 'sqlite', but SQLite support is disabled or SQLite3 was not found. Please install libsqlite3-dev and enable CTRACE_ENABLE_SQLITE, or select a supported backend (stdout, bin, ctf).")
        elseif(ACTIVE_BACKEND STREQUAL "bin-zstd")
            message(FATAL_ERROR "ctrace error: Target '${TARGET_NAME}' requested backend 'bin-zstd', but Zstandard support is disabled or zstd was not found. Please install libzstd-dev and enable CTRACE_ENABLE_ZSTD, or select a supported backend (stdout, bin, ctf).")
        else()
            message(FATAL_ERROR "ctrace error: Target '${TARGET_NAME}' requested unknown or unavailable backend '${ACTIVE_BACKEND}'.")
        endif()
    endif()

    # 1. Require compile_commands.json for Clang LibTooling
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON PARENT_SCOPE)

    get_target_property(TARGET_SOURCES ${TARGET_NAME} SOURCES)
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(TRACE_SIDECARS "")

    # 2. Per-source AST scan step (incremental per .c file)
    foreach(SRC ${TARGET_SOURCES})
        if(SRC MATCHES "\\.c$")
            get_filename_component(SRC_ABS "${SRC}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            get_filename_component(SRC_NAME "${SRC}" NAME)
            set(SIDECAR "${GEN_DIR}/${SRC_NAME}.trace.json")

            add_custom_command(
                OUTPUT "${SIDECAR}"
                COMMAND $<TARGET_FILE:${CTRACE_SCAN_TARGET}>
                        -p "${CMAKE_BINARY_DIR}"
                        --extra-arg=-DCTRACE_AST_SCAN
                        --extra-arg=-I${CTRACE_INCLUDE_DIR}
                        -o "${SIDECAR}"
                        "${SRC_ABS}"
                DEPENDS "${SRC_ABS}" ${CTRACE_SCAN_TARGET} "${CMAKE_BINARY_DIR}/compile_commands.json"
                COMMENT "[ctrace] Scanning AST for ${SRC_NAME}"
                VERBATIM
            )
            list(APPEND TRACE_SIDECARS "${SIDECAR}")
        endif()
    endforeach()

    # 3. Merge sidecars & generate backend-specific header
    set(GEN_HEADER "${GEN_DIR}/trace_generated.h")
    set(GEN_SCHEMA "${GEN_DIR}/trace_schema.json")
    set(GEN_METADATA "${GEN_DIR}/metadata")

    add_custom_command(
        OUTPUT "${GEN_HEADER}" "${GEN_SCHEMA}"
        COMMAND $<TARGET_FILE:${CTRACE_GEN_TARGET}>
                --backend=${ACTIVE_BACKEND}
                --out-header="${GEN_HEADER}"
                --out-schema="${GEN_SCHEMA}"
                --out-metadata="${GEN_METADATA}"
                ${TRACE_SIDECARS}
        DEPENDS ${TRACE_SIDECARS} ${CTRACE_GEN_TARGET}
        COMMENT "[ctrace] Generating ${ACTIVE_BACKEND} emitters for ${TARGET_NAME}"
        VERBATIM
    )

    # 4. Wire generated header & backend runtime library into target
    add_custom_target(${TARGET_NAME}_ctrace_codegen DEPENDS "${GEN_HEADER}")
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_ctrace_codegen)

    target_include_directories(${TARGET_NAME} PRIVATE "${CTRACE_INCLUDE_DIR}" "${GEN_DIR}")
    if(ACTIVE_BACKEND STREQUAL "sqlite")
        target_compile_definitions(${TARGET_NAME} PRIVATE CTRACE_BACKEND_SQLITE)
    endif()

    target_link_libraries(${TARGET_NAME} PRIVATE ${CTRACE_RT_TARGET})
endfunction()
