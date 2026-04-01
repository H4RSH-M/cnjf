cmake_minimum_required(VERSION 3.14)
project(GameNetworkingEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(src include .)
find_package(OpenSSL REQUIRED)

# RAYLIB AUTO-DOWNLOADER

include(FetchContent)
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.0.tar.gz
)
FetchContent_MakeAvailable(raylib)

# SERVER EXECUTABLE (Linux)

if(UNIX)
    message(STATUS "Linux detected: Building the Game Server...")
    add_executable(server src/server_main.cpp)
    target_link_libraries(server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# CLIENT EXECUTABLE (Universal with GUI)

add_executable(client src/client_main.cpp)
target_link_libraries(client PRIVATE OpenSSL::SSL OpenSSL::Crypto raylib)

if(WIN32)
    message(STATUS "Windows detected: Linking Winsock and Windows Media libraries...")
    target_link_libraries(client PRIVATE ws2_32 winmm)
endif()