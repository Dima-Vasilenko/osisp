cmake_minimum_required(VERSION 3.16)
project(lab06_regwatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lab06_regwatch
    src/main.cpp
    src/registry.cpp
    src/monitor.cpp
)
target_compile_options(lab06_regwatch PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME lab06_checks
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_checks.py
    )
    set_tests_properties(lab06_checks PROPERTIES
        ENVIRONMENT "LAB06_BIN=$<TARGET_FILE:lab06_regwatch>"
    )
endif()
