find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(BABELTRACE2_EXEC NAMES babeltrace2 babeltrace)

# Test 1: stdout backend
add_executable(test_stdout test_stdout.c)
target_include_directories(test_stdout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_stdout BACKEND stdout)
add_test(NAME test_stdout COMMAND test_stdout)

# Test 2: bin backend
add_executable(test_bin test_bin.c)
target_include_directories(test_bin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_bin BACKEND bin)
add_test(NAME test_bin COMMAND test_bin)
add_test(NAME test_bin_decode
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/ctrace_decode.py
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_bin/trace_schema.json
            test_bin.bin
)
set_tests_properties(test_bin_decode PROPERTIES DEPENDS test_bin)

add_test(NAME test_bin_decode_cpp
    COMMAND $<TARGET_FILE:ctrace-decode>
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_bin/trace_schema.json
            test_bin.bin
)
set_tests_properties(test_bin_decode_cpp PROPERTIES DEPENDS test_bin)

add_test(NAME test_decode_edge_cases
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_decode_edge_cases.py
            $<TARGET_FILE:ctrace-decode>
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_bin/trace_schema.json
            ${PROJECT_SOURCE_DIR}/tools/ctrace_decode.py
)
set_tests_properties(test_decode_edge_cases PROPERTIES DEPENDS test_bin)

# Test 3: bin-zstd backend (optional)
if(TARGET ctrace_rt_bin-zstd)
    add_executable(test_zstd test_zstd.c)
    target_include_directories(test_zstd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_enable_ctrace(test_zstd BACKEND bin-zstd)
    add_test(NAME test_zstd COMMAND test_zstd)
    add_test(NAME test_zstd_decode
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/ctrace_decode.py
                ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_zstd/trace_schema.json
                test_zstd.bin.zst
    )
    set_tests_properties(test_zstd_decode PROPERTIES DEPENDS test_zstd)

    add_test(NAME test_zstd_decode_cpp
        COMMAND $<TARGET_FILE:ctrace-decode>
                ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_zstd/trace_schema.json
                test_zstd.bin.zst
    )
    set_tests_properties(test_zstd_decode_cpp PROPERTIES DEPENDS test_zstd)
endif()

# Test 4: sqlite backend (optional)
if(TARGET ctrace_rt_sqlite)
    add_executable(test_sqlite test_sqlite.c)
    target_include_directories(test_sqlite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_enable_ctrace(test_sqlite BACKEND sqlite)
    add_test(NAME test_sqlite COMMAND test_sqlite)
endif()

# Test 5: multi translation units
add_executable(test_multi test_multi_main.c test_multi_a.c test_multi_b.c)
target_include_directories(test_multi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_multi BACKEND stdout)
add_test(NAME test_multi COMMAND test_multi)

# Test 6: CTF backend (Common Trace Format)
add_executable(test_ctf test_ctf.c)
target_include_directories(test_ctf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_ctf BACKEND ctf)
add_test(NAME test_ctf COMMAND test_ctf)

if(BABELTRACE2_EXEC)
    add_test(NAME test_ctf_babeltrace
        COMMAND ${BABELTRACE2_EXEC} test_ctf_trace
    )
    set_tests_properties(test_ctf_babeltrace PROPERTIES DEPENDS test_ctf)
    add_test(NAME test_ctf_verify
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_ctf.py
                ${BABELTRACE2_EXEC} test_ctf_trace
    )
    set_tests_properties(test_ctf_verify PROPERTIES DEPENDS test_ctf)
endif()

# Test 7: Complex types (deeply nested structs, fixed-size arrays, struct pointers)
add_executable(test_complex test_complex.c)
target_include_directories(test_complex PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_complex BACKEND stdout)
add_test(NAME test_complex COMMAND test_complex)

# Test 8: CountedBy dynamic arrays (1-layer FAM and 2-layer nested dynamic arrays)
add_executable(test_counted_by test_counted_by.c)
target_include_directories(test_counted_by PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_counted_by BACKEND stdout)
add_test(NAME test_counted_by COMMAND test_counted_by)

# Test 9: Background writer thread controls (name, RT guard, affinity, priority, multi-wrap)
add_executable(test_writer_thread test_writer_thread.c)
target_include_directories(test_writer_thread PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_writer_thread BACKEND bin)
add_test(NAME test_writer_thread COMMAND test_writer_thread)
add_test(NAME test_writer_thread_decode
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/ctrace_decode.py
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_writer_thread/trace_schema.json
            test_writer.bin
)
set_tests_properties(test_writer_thread_decode PROPERTIES DEPENDS test_writer_thread)

add_test(NAME test_writer_thread_decode_cpp
    COMMAND $<TARGET_FILE:ctrace-decode>
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_writer_thread/trace_schema.json
            test_writer.bin
)
set_tests_properties(test_writer_thread_decode_cpp PROPERTIES DEPENDS test_writer_thread)

# Test 10: CPM package manager integration test
set(CPM_TEST_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/cpm_build")
add_test(NAME test_cpm_configure
    COMMAND ${CMAKE_COMMAND} -B ${CPM_TEST_BUILD_DIR} -S ${CMAKE_CURRENT_SOURCE_DIR}/cpm_test
)
add_test(NAME test_cpm_build
    COMMAND ${CMAKE_COMMAND} --build ${CPM_TEST_BUILD_DIR}
)
set_tests_properties(test_cpm_build PROPERTIES DEPENDS test_cpm_configure)

add_test(NAME test_cpm_run
    COMMAND ${CPM_TEST_BUILD_DIR}/cpm_consumer
    WORKING_DIRECTORY ${CPM_TEST_BUILD_DIR}
)
set_tests_properties(test_cpm_run PROPERTIES DEPENDS test_cpm_build)

# Test 11: FAPI unions, fixed arrays, and dynamic count binding
add_executable(test_fapi test_fapi.c)
target_include_directories(test_fapi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_fapi BACKEND bin)
add_test(NAME test_fapi COMMAND test_fapi)
add_test(NAME test_fapi_verify
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_fapi.py
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_fapi/trace_schema.json
            test_fapi.bin
            ${PROJECT_SOURCE_DIR}/tools/ctrace_decode.py
)
set_tests_properties(test_fapi_verify PROPERTIES DEPENDS test_fapi)

add_test(NAME test_fapi_verify_cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/verify_fapi.py
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_fapi/trace_schema.json
            test_fapi.bin
            $<TARGET_FILE:ctrace-decode>
)
set_tests_properties(test_fapi_verify_cpp PROPERTIES DEPENDS test_fapi)

# Test 11b: Discriminated unions, union pointers and anonymous members on the stdout backend
add_executable(test_unions_stdout test_unions_stdout.c)
target_include_directories(test_unions_stdout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_unions_stdout BACKEND stdout)
add_test(NAME test_unions_stdout COMMAND test_unions_stdout)

# Test 12: Runtime multi-backend selection via CTRACE_BACKEND env var
add_executable(test_runtime test_runtime.c)
target_include_directories(test_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(test_runtime BACKEND runtime)

add_test(NAME test_runtime_stdout COMMAND test_runtime)
set_tests_properties(test_runtime_stdout PROPERTIES
    ENVIRONMENT "CTRACE_BACKEND=stdout"
)

add_test(NAME test_runtime_bin COMMAND test_runtime)
set_tests_properties(test_runtime_bin PROPERTIES
    ENVIRONMENT "CTRACE_BACKEND=bin;CTRACE_OUTPUT_PATH=test_runtime.bin"
)

add_test(NAME test_runtime_bin_decode
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/ctrace_decode.py
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_runtime/trace_schema.json
            test_runtime.bin
)
set_tests_properties(test_runtime_bin_decode PROPERTIES DEPENDS test_runtime_bin)

add_test(NAME test_runtime_bin_decode_cpp
    COMMAND $<TARGET_FILE:ctrace-decode>
            ${CMAKE_CURRENT_BINARY_DIR}/ctrace_gen_test_runtime/trace_schema.json
            test_runtime.bin
)
set_tests_properties(test_runtime_bin_decode_cpp PROPERTIES DEPENDS test_runtime_bin)
