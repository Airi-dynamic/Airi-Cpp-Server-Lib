cmake_minimum_required(VERSION 3.10)
project(day29 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# Poller 源文件：按平台选择
set(POLLER_SRCS common/Poller/DefaultPoller.cpp)
if(APPLE)
    list(APPEND POLLER_SRCS common/Poller/kqueue/KqueuePoller.cpp)
else()
    list(APPEND POLLER_SRCS common/Poller/epoll/EpollPoller.cpp)
endif()

set(LOG_SRC
    common/log/AsyncLogging.cpp
    common/log/LogFile.cpp
    common/log/LogStream.cpp
    common/log/Logger.cpp
)

set(HTTP_SRC
    common/http/HttpContext.cpp
    common/http/HttpRequest.cpp
    common/http/HttpResponse.cpp
    common/http/HttpServer.cpp
)

set(COMMON_SRC
    common/Acceptor.cpp
    common/Buffer.cpp
    common/Channel.cpp
    common/Connection.cpp
    common/EventLoopThread.cpp
    common/EventLoopThreadPool.cpp
    common/Eventloop.cpp
    common/InetAddress.cpp
    common/TcpServer.cpp
    common/Socket.cpp
    common/ThreadPool.cpp
    common/timer/TimerQueue.cpp
    ${LOG_SRC}
    ${HTTP_SRC}
    ${POLLER_SRCS}
)

# ── 手工测试可执行文件 ──
add_executable(server test/server.cpp ${COMMON_SRC})
target_link_libraries(server pthread)

add_executable(client test/client.cpp)

# ── 单元/策略测试 ──
add_executable(BackpressureDecisionTest test/BackpressureDecisionTest.cpp ${COMMON_SRC})
target_link_libraries(BackpressureDecisionTest pthread)

add_executable(EpollPolicyTest test/EpollPolicyTest.cpp ${COMMON_SRC})
target_link_libraries(EpollPolicyTest pthread)

add_executable(HttpContextTest test/HttpContextTest.cpp ${COMMON_SRC})
target_link_libraries(HttpContextTest pthread)

add_executable(HttpRequestLimitsTest test/HttpRequestLimitsTest.cpp ${COMMON_SRC})
target_link_libraries(HttpRequestLimitsTest pthread)

add_executable(SocketPolicyTest test/SocketPolicyTest.cpp ${COMMON_SRC})
target_link_libraries(SocketPolicyTest pthread)

add_executable(TcpServerPolicyTest test/TcpServerPolicyTest.cpp ${COMMON_SRC})
target_link_libraries(TcpServerPolicyTest pthread)

add_executable(TimerTest test/TimerTest.cpp ${COMMON_SRC})
target_link_libraries(TimerTest pthread)

add_executable(LogTest test/LogTest.cpp ${COMMON_SRC})
target_link_libraries(LogTest pthread)

add_executable(ThreadPoolTest test/ThreadPoolTest.cpp common/ThreadPool.cpp)
target_link_libraries(ThreadPoolTest pthread)

add_executable(StressTest
    test/StressTest.cpp
    common/Buffer.cpp
    common/InetAddress.cpp
    common/Socket.cpp
    ${LOG_SRC}
)

add_executable(BenchmarkTest test/BenchmarkTest.cpp)
target_link_libraries(BenchmarkTest pthread)

# ─────────────────────────────────────────────────────────────────────────
# Day 28 引入的 CTest 注册（day29 起继承并补充 HttpRequestLimitsTest）
# ─────────────────────────────────────────────────────────────────────────
enable_testing()
add_test(NAME BackpressureDecisionTest COMMAND BackpressureDecisionTest)
add_test(NAME TcpServerPolicyTest      COMMAND TcpServerPolicyTest)
add_test(NAME SocketPolicyTest         COMMAND SocketPolicyTest)
add_test(NAME HttpContextTest          COMMAND HttpContextTest)
add_test(NAME HttpRequestLimitsTest    COMMAND HttpRequestLimitsTest)
add_test(NAME EpollPolicyTest          COMMAND EpollPolicyTest)
add_test(NAME LogTest                  COMMAND LogTest)
add_test(NAME TimerTest                COMMAND TimerTest)
add_test(NAME ThreadPoolTest           COMMAND ThreadPoolTest)

# app_example/ 有独立的 CMakeLists.txt，需单独构建
