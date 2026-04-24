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
