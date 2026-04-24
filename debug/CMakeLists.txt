cmake_minimum_required(VERSION 3.14)
project(DebugLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(USE_ASAN  "Enable AddressSanitizer (UAF, overflow, leak)"  OFF)
option(USE_TSAN  "Enable ThreadSanitizer (data race, deadlock)"   OFF)
option(USE_UBSAN "Enable UndefinedBehaviorSanitizer (UB)"         OFF)

if(USE_ASAN AND USE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled simultaneously!")
endif()

# -g: debug symbols (for GDB/LLDB/perf)
# -O1: light optimization (sanitizers work best with -O1)
# -fno-omit-frame-pointer: accurate stack traces for perf/flamegraph
add_compile_options(-g -O1 -fno-omit-frame-pointer)

if(USE_ASAN)
    message(STATUS ">>> AddressSanitizer ENABLED")
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()

if(USE_TSAN)
    message(STATUS ">>> ThreadSanitizer ENABLED")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

if(USE_UBSAN)
    message(STATUS ">>> UndefinedBehaviorSanitizer ENABLED")
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()

add_executable(buggy_server buggy_server.cpp)
target_link_libraries(buggy_server PRIVATE pthread)

add_executable(debugged_server debugged_server.cpp)
target_link_libraries(debugged_server PRIVATE pthread)
