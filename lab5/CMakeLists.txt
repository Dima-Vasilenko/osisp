cmake_minimum_required(VERSION 3.16)
project(lab05_peer_chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(net_common STATIC
    src/common/framing.cpp
    src/common/logger.cpp
    src/common/protocol.cpp
    src/common/socket_utils.cpp
)
target_include_directories(net_common PUBLIC src)
target_compile_options(net_common PRIVATE -Wall -Wextra -Wpedantic)

add_executable(peer_chat
    src/peer/main.cpp
    src/peer/options.cpp
    src/peer/peer_node.cpp
)
target_link_libraries(peer_chat PRIVATE net_common Threads::Threads)
target_compile_options(peer_chat PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME lab05_checks
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_checks.py
    )
    set_tests_properties(lab05_checks PROPERTIES
        ENVIRONMENT "LAB05_BIN=$<TARGET_FILE:peer_chat>"
    )
endif()
