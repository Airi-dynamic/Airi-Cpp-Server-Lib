cmake_minimum_required(VERSION 3.10)
project(day05 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(server
    server.cpp
    common/Channel.cpp
    common/Epoll.cpp
    common/InetAddress.cpp
    common/Socket.cpp
    common/util.cpp
)

add_executable(client
    client.cpp
    common/util.cpp
)
