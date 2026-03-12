cmake_minimum_required(VERSION 3.10)
project(JackfruitEngine VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(OpenSSL REQUIRED)

# Tell the compiler where to find headers
include_directories(${OPENSSL_INCLUDE_DIR} include)

# server exec
add_executable(server src/server_main.cpp)
target_link_libraries(server PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# client executable
add_executable(client src/client_main.cpp)
target_link_libraries(client PRIVATE OpenSSL::SSL OpenSSL::Crypto)