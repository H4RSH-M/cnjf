cmake_minimum_required(VERSION 3.14)
project(GameNetworkingEngine)

# shut up the fetchcontent timestamp warning
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(src include .)

# pull raylib from github
include(FetchContent)
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.0.tar.gz
    TLS_VERIFY OFF
)
FetchContent_MakeAvailable(raylib)

# linux build (server and client)
if(UNIX)
    message(STATUS "Linux detected: Building with standard package manager...")
    find_package(OpenSSL REQUIRED)
    
    # server
    add_executable(server src/server_main.cpp)
    target_link_libraries(server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    
    # client
    add_executable(client src/client_main.cpp)
    target_link_libraries(client PRIVATE OpenSSL::SSL OpenSSL::Crypto raylib)
endif()

# windows build (client only - bypass cmake search and hardlink openssl)
if(WIN32)
    message(STATUS "Windows detected: Bypassing CMake search and hardlinking OpenSSL...")
    
    add_executable(client src/client_main.cpp)
    
    # force include path from the new location
    target_include_directories(client PRIVATE "C:/OpenSSL-Win64/include")
    
    # force exact .lib files and windows networking
    target_link_libraries(client PRIVATE 
        raylib 
        ws2_32 
        winmm 
        "C:/OpenSSL-Win64/lib/VC/x64/MD/libssl.lib" #clients should replace with their particular path for these two files
        "C:/OpenSSL-Win64/lib/VC/x64/MD/libcrypto.lib"
    )
endif()