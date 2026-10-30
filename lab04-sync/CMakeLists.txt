cmake_minimum_required(VERSION 3.16)
project(lab04_sync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(lab04_sync src/main.cpp)
target_compile_options(lab04_sync PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(lab04_sync PRIVATE Threads::Threads)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    enable_testing()
    add_test(
        NAME lab04_checks
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_checks.py
    )
    set_tests_properties(lab04_checks PROPERTIES
        ENVIRONMENT "LAB04_BIN=$<TARGET_FILE:lab04_sync>"
    )
endif()
