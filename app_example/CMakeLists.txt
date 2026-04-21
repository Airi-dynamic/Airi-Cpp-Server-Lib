cmake_minimum_required(VERSION 3.10)
project(AppExample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────────────────────────────────────────────────────────────────
# Day 29 起 app_example 沿用 day28 的"相对路径直链父目录源码"模式：
# 不再把库头/导出 cmake 复制到 external/，避免任何重复或漂移。
# 新增的 CorsMiddleware / GzipMiddleware / StaticFileHandler 都是仅头实现，
# 因此 LIB_SRCS 在 day28 基础上仅追加 day29 复活的 util.cpp。
# ─────────────────────────────────────────────────────────────────────────

set(LIB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/DefaultPoller.cpp)
if(APPLE)
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND LIB_POLLER_SRCS ${LIB_ROOT}/common/Poller/epoll/EpollPoller.cpp)
endif()

set(LIB_SRCS
    ${LIB_ROOT}/common/Acceptor.cpp
    ${LIB_ROOT}/common/Buffer.cpp
    ${LIB_ROOT}/common/Channel.cpp
    ${LIB_ROOT}/common/Connection.cpp
    ${LIB_ROOT}/common/EventLoopThread.cpp
    ${LIB_ROOT}/common/EventLoopThreadPool.cpp
    ${LIB_ROOT}/common/Eventloop.cpp
    ${LIB_ROOT}/common/InetAddress.cpp
    ${LIB_ROOT}/common/Socket.cpp
    ${LIB_ROOT}/common/TcpServer.cpp
    ${LIB_ROOT}/common/ThreadPool.cpp
    ${LIB_ROOT}/common/timer/TimerQueue.cpp
    ${LIB_ROOT}/common/log/AsyncLogging.cpp
    ${LIB_ROOT}/common/log/LogFile.cpp
    ${LIB_ROOT}/common/log/LogStream.cpp
    ${LIB_ROOT}/common/log/Logger.cpp
    ${LIB_ROOT}/common/http/HttpContext.cpp
    ${LIB_ROOT}/common/http/HttpRequest.cpp
    ${LIB_ROOT}/common/http/HttpResponse.cpp
    ${LIB_ROOT}/common/http/HttpServer.cpp
    ${LIB_ROOT}/common/util.cpp
    ${LIB_POLLER_SRCS}
)

find_package(ZLIB QUIET)

add_executable(http_server src/http_server.cpp ${LIB_SRCS})
target_include_directories(http_server PRIVATE ${LIB_ROOT}/include)
target_link_libraries(http_server PRIVATE pthread)

if(ZLIB_FOUND)
    target_link_libraries(http_server PRIVATE ZLIB::ZLIB)
    target_compile_definitions(http_server PRIVATE HAS_ZLIB=1)
endif()

# 运行期需要的静态资源：拷贝到构建目录
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/static DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/files DESTINATION ${CMAKE_BINARY_DIR})
cmake_minimum_required(VERSION 3.21)

project(AppExample LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================================================
# 最关键的一步：告诉 CMake 到 external/Airi-Cpp-Server-Lib 目录下去寻找第三方库
# =========================================================================
set(CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/Airi-Cpp-Server-Lib" ${CMAKE_PREFIX_PATH})

# 查找我们刚才通过 install 安装好的库
find_package(Airi-Cpp-Server-Lib 1.0.0 REQUIRED)
find_package(ZLIB QUIET)

# 编译应用
add_executable(http_server src/http_server.cpp)

# 链接网络库的导出目标
target_link_libraries(http_server PRIVATE Airi-Cpp-Server-Lib::NetLib pthread)

if(ZLIB_FOUND)
	target_link_libraries(http_server PRIVATE ZLIB::ZLIB)
	target_compile_definitions(http_server PRIVATE HAS_ZLIB=1)
endif()

# 将必要的静态资源文件拷贝到 build 目录，以便程序运行时能找到文件
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/static DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/files DESTINATION ${CMAKE_BINARY_DIR})
