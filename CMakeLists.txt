cmake_minimum_required(VERSION 3.16)
project(MyRPCProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(RPC_CORE_SOURCES
    src/Logger.cpp
    src/Buffer.cpp
    src/Channel.cpp
    src/Poller.cpp
    src/PollPoller.cpp
    src/EventLoop.cpp
    src/Codec.cpp
    src/AIService.cpp
    src/TaskCoordinator.cpp
    src/Metrics.cpp
    src/ThreadPool.cpp
    src/Acceptor.cpp
    src/TcpConnection.cpp
    src/TcpServer.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND RPC_CORE_SOURCES src/EpollPoller.cpp)
endif()

add_library(rpc_core ${RPC_CORE_SOURCES})

target_include_directories(rpc_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rpc_core
    PUBLIC
        Threads::Threads
)

add_executable(rpc_demo
    src/main.cpp
)

target_link_libraries(rpc_demo
    PRIVATE
        rpc_core
        Threads::Threads
)
