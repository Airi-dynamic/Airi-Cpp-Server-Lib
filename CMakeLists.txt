cmake_minimum_required(VERSION 3.10)
project(day26 LANGUAGES CXX)

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
    common/util.cpp
    ${LOG_SRC}
    ${HTTP_SRC}
    ${POLLER_SRCS}
)

add_executable(server server.cpp ${COMMON_SRC})
target_link_libraries(server pthread)

add_executable(client client.cpp common/util.cpp)

add_executable(http_server http_server.cpp ${COMMON_SRC})
target_link_libraries(http_server pthread)

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
    common/util.cpp
)

add_executable(BenchmarkTest test/BenchmarkTest.cpp)
target_link_libraries(BenchmarkTest pthread)

# 将静态资源和数据文件拷贝到 build 目录
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/static DESTINATION ${CMAKE_BINARY_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/files DESTINATION ${CMAKE_BINARY_DIR})
