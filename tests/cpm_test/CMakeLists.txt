cmake_minimum_required(VERSION 3.20)
project(cpm_test_project C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(cmake/CPM.cmake)

# Import ctrace via CPM from local source directory
CPMAddPackage(
    NAME ctrace
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

add_executable(cpm_consumer main.c)
target_include_directories(cpm_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_enable_ctrace(cpm_consumer BACKEND bin)
