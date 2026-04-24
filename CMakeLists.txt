cmake_minimum_required(VERSION 3.21)

# 生成 compile_commands.json，供 VS Code 的 C/C++ 插件和 clang-tidy 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 项目信息
project(Airi-Cpp-Server-Lib VERSION 1.0.0 LANGUAGES CXX)

option(MCPP_ENABLE_OPENSSL "Enable TLS support via OpenSSL" ON)
option(MCPP_ENABLE_TESTING "Build tests (requires GoogleTest via FetchContent)" ON)
option(MCPP_ENABLE_ASAN    "Enable AddressSanitizer"  OFF)
option(MCPP_ENABLE_TSAN    "Enable ThreadSanitizer"   OFF)
option(MCPP_ENABLE_UBSAN   "Enable UndefinedBehaviorSanitizer" OFF)
option(MCPP_ENABLE_COVERAGE "Enable code coverage (gcov / llvm-cov)" OFF)

# day31-36 的加分实验（WebSocket / 协程 / io_uring / 无锁队列 / 内存池）仅作为实验分支保留在
# HISTORY/day31…day36/ 中供参考，未合入本主代码树。请勿重新启用下面这些开关：
#   MCPP_ENABLE_COROUTINES、MCPP_ENABLE_IO_URING

# C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sanitizer flags ─────────────────────────────────────────────
if(MCPP_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(MCPP_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
if(MCPP_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if(MCPP_ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# 头文件路径 - 使用 target_include_directories 而不是全局 include_directories
# 稍后在 NetLib 目标定义之后设置 PUBLIC/PRIVATE include 目录

# 扫描所有公共源文件（排除平台专有目录和不需要打包的文件）
file(GLOB_RECURSE COMMON_SOURCES "src/common/*.cpp")
list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/Poller/epoll/.*")
list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/Poller/kqueue/.*")
list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/EventLoop/epoll/.*")
list(FILTER COMMON_SOURCES EXCLUDE REGEX ".*/EventLoop/kqueue/.*")

# 按平台加入唯一的那个实现文件
set(PLATFORM_SOURCES "")
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    list(APPEND PLATFORM_SOURCES
        "src/common/net/Poller/epoll/EpollPoller.cpp")
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    list(APPEND PLATFORM_SOURCES
        "src/common/net/Poller/kqueue/KqueuePoller.cpp")
else()
    message(FATAL_ERROR "Unsupported OS: ${CMAKE_SYSTEM_NAME}")
endif()

# 1. 核心库 (将决定权交给使用者)
add_library(NetLib ${COMMON_SOURCES} ${PLATFORM_SOURCES})
set_target_properties(NetLib PROPERTIES OUTPUT_NAME "Airi-Cpp-Server-Lib")
add_library(Airi-Cpp-Server-Lib::NetLib ALIAS NetLib)

set(MYCPPSERVERLIB_WITH_OPENSSL OFF)
if(MCPP_ENABLE_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(NetLib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(NetLib PUBLIC MCPP_HAS_OPENSSL=1)
        set(MYCPPSERVERLIB_WITH_OPENSSL ON)
        message(STATUS "Airi-Cpp-Server-Lib: OpenSSL found, TLS support enabled")
    else()
        message(STATUS "Airi-Cpp-Server-Lib: OpenSSL not found, TLS support disabled")
    endif()
endif()

# zlib 链接与宏（gzip 中间件需要）
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    target_link_libraries(NetLib PUBLIC ZLIB::ZLIB)
    target_compile_definitions(NetLib PUBLIC MCPP_HAS_ZLIB=1)
    message(STATUS "Airi-Cpp-Server-Lib: zlib found, gzip middleware enabled")
else()
    message(STATUS "Airi-Cpp-Server-Lib: zlib not found, gzip middleware disabled")
endif()

# 设置头文件搜索路径：构建时(BUILD_INTERFACE)和安装后(INSTALL_INTERFACE)
target_include_directories(NetLib 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/net>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/net/Poller>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/timer>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/log>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/http>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/async>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/include/memory>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/net>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/net/Poller>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/timer>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/log>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/http>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/async>
        $<INSTALL_INTERFACE:include/Airi-Cpp-Server-Lib/memory>
)

# 【新增】安装规则（打包输出，支持 find_package(Airi-Cpp-Server-Lib)）
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 1. 安装库文件，并导出 Target (EXPORT)
install(TARGETS NetLib 
    EXPORT Airi-Cpp-Server-LibTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Airi-Cpp-Server-Lib
)

# 2. 安装头文件到 include/Airi-Cpp-Server-Lib
install(DIRECTORY src/include/ 
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Airi-Cpp-Server-Lib 
        FILES_MATCHING PATTERN "*.h")

# 3. 安装导出的 Targets 文件，使得外部可以通过 Airi-Cpp-Server-Lib::NetLib 链接
install(EXPORT Airi-Cpp-Server-LibTargets
    FILE Airi-Cpp-Server-LibTargets.cmake
    NAMESPACE Airi-Cpp-Server-Lib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Airi-Cpp-Server-Lib
)

# 4. 生成 & 安装 Config 配置文件（如 Airi-Cpp-Server-LibConfig.cmake 和 版本文件）
write_basic_package_version_file(
    "Airi-Cpp-Server-LibConfigVersion.cmake"
    VERSION ${Airi-Cpp-Server-Lib_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Airi-Cpp-Server-LibConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Airi-Cpp-Server-LibConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Airi-Cpp-Server-Lib
)

install(FILES 
    "${CMAKE_CURRENT_BINARY_DIR}/Airi-Cpp-Server-LibConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/Airi-Cpp-Server-LibConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Airi-Cpp-Server-Lib
)

# 2. Demo / manual-run programs
add_executable(StressTest src/test/StressTest.cpp)
target_link_libraries(StressTest NetLib pthread)

add_executable(TimerTest src/test/TimerTest.cpp)
target_link_libraries(TimerTest NetLib pthread)

add_executable(LogTest src/test/LogTest.cpp)
target_link_libraries(LogTest NetLib pthread)

# 注：性能基准测试不再使用自研客户端，统一改为业界标准 wrk + Docker 真实对照
# 见 benchmark/muduo_compare/  (跑 wrk 压测，对比 muduo)

# Phase 4 演示服务器
add_executable(demo_server demo/demo_server.cpp)
target_link_libraries(demo_server NetLib pthread)

# 全特性演示服务器（中间件链 / 限流 / 鉴权 / CORS / gzip / 静态文件）
add_executable(app_demo examples/src/http_server.cpp)
target_link_libraries(app_demo NetLib pthread)
if(ZLIB_FOUND)
    target_link_libraries(app_demo ZLIB::ZLIB)
    target_compile_definitions(app_demo PRIVATE HAS_ZLIB=1)
endif()
# 复制静态资源到 build 目录，使 ./app_demo 在 build/ 下能找到 static/、files/
add_custom_command(TARGET app_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/examples/static ${CMAKE_BINARY_DIR}/static
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/examples/files  ${CMAKE_BINARY_DIR}/files)

# ── GoogleTest + CTest ──────────────────────────────────────────
if(MCPP_ENABLE_TESTING)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    # 不安装 gtest 到系统路径
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    # 辅助函数：注册一个 gtest 可执行文件
    function(mcpp_add_test NAME)
        add_executable(${NAME} src/test/${NAME}.cpp)
        target_link_libraries(${NAME} PRIVATE NetLib GTest::gtest_main pthread)
        gtest_discover_tests(${NAME})
    endfunction()

    mcpp_add_test(HttpContextTest)
    mcpp_add_test(HttpRequestLimitsTest)
    mcpp_add_test(BackpressureDecisionTest)
    mcpp_add_test(MetricsTest)
    mcpp_add_test(LogContextTest)
    mcpp_add_test(CorsMiddlewareTest)
    mcpp_add_test(StaticFileHandlerTest)
endif()

# ── 静态分析：clang-tidy（可选 custom target）────────────────────
find_program(CLANG_TIDY_EXE NAMES clang-tidy)
if(CLANG_TIDY_EXE)
    file(GLOB_RECURSE ALL_LINT_SOURCES
        "src/common/*.cpp" "src/common/*.h"
        "src/include/*.h")
    add_custom_target(clang-tidy
        COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            ${ALL_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy static analysis"
        VERBATIM
    )
endif()

# 让 clangd / 其他 LSP 在仓库根目录也能稳定找到编译数据库。
# 这样项目目录迁移后，只要重新 configure/build 一次，IDE 诊断就会自动恢复。
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    add_custom_target(sync_compile_commands ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_BINARY_DIR}/compile_commands.json
            ${CMAKE_SOURCE_DIR}/compile_commands.json
        COMMENT "Sync compile_commands.json to workspace root"
    )
endif()