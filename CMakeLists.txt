cmake_minimum_required(VERSION 3.10)
project(day23 LANGUAGES CXX)

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
    ${POLLER_SRCS}
)

add_executable(server server.cpp ${COMMON_SRC})
target_link_libraries(server pthread)

add_executable(client client.cpp common/util.cpp)

add_executable(TimerTest test/TimerTest.cpp ${COMMON_SRC})
target_link_libraries(TimerTest pthread)

add_executable(ThreadPoolTest test/ThreadPoolTest.cpp common/ThreadPool.cpp)
target_link_libraries(ThreadPoolTest pthread)

add_executable(StressTest
    test/StressTest.cpp
    common/Buffer.cpp
    common/InetAddress.cpp
    common/Socket.cpp
    common/util.cpp
)
